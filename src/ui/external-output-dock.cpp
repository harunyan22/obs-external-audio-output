/*
Plugin Name
Copyright (C) 2026 minimamu

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

// 対応 REQ: REQ-UI-001, REQ-UI-004, REQ-UI-005, REQ-UI-006, REQ-UI-007, REQ-UI-008, REQ-AUDIO-006
// / Slice 5

#include "external-output-dock.hpp"

#include "../audio/track-map.hpp"
#include "../config/plugin-config.hpp"
#include "../device/audio-device-manager.hpp"
#include "../diag/diagnostics-text.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStringList>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace {

// obs_module_text() の結果は UTF-8(D-19、data/locale/*.ini)。QString へは fromUtf8 で変換する。
QString Tr(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// Level Meter の表示下限(D-15、REQUIREMENTS.md §5: -60〜0dBFS)。
constexpr float kMeterFloorDb = -60.0f;
// UI 側の内部フロア(-60dB より低い値も保持する。無音判定(AC-13: -60dBFS 未満)を安定して
// 表現するため、表示クランプとは別に少し低い値まで計算上は許容する)。
constexpr float kMeterInternalFloorDb = -100.0f;

float LinearToDbfs(float linear)
{
	if (linear <= 0.0f)
		return kMeterInternalFloorDb;
	return 20.0f * std::log10(linear);
}

// レビュー最終指摘(Low-5): was_running(RollbackCompatModeToIndependent() へ渡す「切替前に独立出力が
// 動いていたか」)の判定を Running だけでなく Starting/Recovering まで広げる。遷移中(接続試行中/
// 再接続中)に output_mode を切り替えたユーザーの意図も「動かしたい」であり、巻き戻し後に独立出力の
// Start() を再 enqueue して復元されるべきため。
bool IsOutputActiveState(output_instance::State state)
{
	return state == output_instance::State::Running || state == output_instance::State::Starting ||
	       state == output_instance::State::Recovering;
}

// クリップ修正(2026-08-31、実機フィードバック追加分→再修正、スクリーンショット検証で再発見):
// QLabel の wordWrap 高さ計算は sizePolicy.hasHeightForWidth() に依存するが、setSizePolicy() へ
// 新規に組んだ QSizePolicy を渡すと(デフォルトで) heightForWidth フラグが立たず、折り返しに
// 必要な高さが QFormLayout/QVBoxLayout の heightForWidth 計算から外れて末尾が見切れる。
// 折り返しラベルへ新規ポリシーを設定する箇所は必ずこのヘルパー経由にし、
// setHeightForWidth(true) を落とさないようにする。
void ApplyWordWrapSizePolicy(QLabel *label, QSizePolicy::Policy horizontal, QSizePolicy::Policy vertical)
{
	QSizePolicy policy(horizontal, vertical);
	policy.setHeightForWidth(true);
	label->setSizePolicy(policy);
}

} // namespace

// ---------------------------------------------------------------------------
// LevelMeterWidget
// ---------------------------------------------------------------------------

LevelMeterWidget::LevelMeterWidget(QWidget *parent) : QWidget(parent)
{
	setMinimumHeight(30);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void LevelMeterWidget::SetLevels(float display_l_db, float display_r_db, float peak_hold_l_db, float peak_hold_r_db)
{
	display_l_db_ = display_l_db;
	display_r_db_ = display_r_db;
	peak_hold_l_db_ = peak_hold_l_db;
	peak_hold_r_db_ = peak_hold_r_db;
	update();
}

void LevelMeterWidget::paintEvent(QPaintEvent *)
{
	QPainter painter(this);

	const int total_height = height();
	const int total_width = width();
	const int bar_height = (total_height - 4) / 2;

	auto DbToFraction = [](float db) {
		float frac = (db - kMeterFloorDb) / (0.0f - kMeterFloorDb);
		if (frac < 0.0f)
			frac = 0.0f;
		if (frac > 1.0f)
			frac = 1.0f;
		return frac;
	};

	auto DrawChannel = [&](int y, float db, float peak_db) {
		painter.fillRect(0, y, total_width, bar_height, QColor(45, 45, 45));

		const float frac = DbToFraction(db);
		const int fill_width = (int)(total_width * frac);
		QColor fill_color;
		if (frac > 0.95f)
			fill_color = QColor(220, 70, 70); // 0dBFS 付近(クリップ注意)
		else if (frac > 0.75f)
			fill_color = QColor(230, 190, 60); // -15dBFS 付近
		else
			fill_color = QColor(70, 180, 100); // 通常域
		if (fill_width > 0)
			painter.fillRect(0, y, fill_width, bar_height, fill_color);

		const float peak_frac = DbToFraction(peak_db);
		const int peak_x = (int)(total_width * peak_frac);
		const int peak_line_x = peak_x > 1 ? peak_x - 1 : 0;
		painter.fillRect(peak_line_x, y, 2, bar_height, QColor(240, 240, 240));
	};

	DrawChannel(0, display_l_db_, peak_hold_l_db_);
	// レビュー指摘 L-14: R バーの同値/無効表示は用意しない。WasapiOutput::OpenDeviceOnRenderThread()
	// が nChannels<2 のデバイスを Open() 失敗(Failed)にするため、Running 中に来る display_r_db_ は
	// 常に実デバイスの R チャンネル由来の値になる(モノラルデバイスの出力に成功することがない)。
	DrawChannel(bar_height + 4, display_r_db_, peak_hold_r_db_);
}

// ---------------------------------------------------------------------------
// DeviceComboBox
// ---------------------------------------------------------------------------

DeviceComboBox::DeviceComboBox(QWidget *parent) : QComboBox(parent) {}

void DeviceComboBox::showPopup()
{
	// REQUIREMENTS.md §5: 「開いたとき/更新ボタンで再列挙」。ポップアップを実際に開く前に
	// ExternalOutputDock に再列挙させる(AudioDeviceManager::Enumerate() は Qt UI スレッド(STA)
	// から安全に呼べる、wasapi-com.md)。
	emit AboutToOpenPopup();
	QComboBox::showPopup();
}

// ---------------------------------------------------------------------------
// ExternalOutputDock
// ---------------------------------------------------------------------------

ExternalOutputDock::ExternalOutputDock(OutputInstance *output_instance, QWidget *parent)
	: QWidget(parent),
	  output_instance_(output_instance),
	  compat_controller_(output_instance)
{
	BuildUi();
	// レビュー指摘 H-2: デバイスコンボの選択確定(実デバイス列挙が必要)は下の
	// OnInitialDeviceEnumeration() へ遅延する。音声トラック/バッファ/自動再接続/ログレベルは
	// 列挙を待たずにここで反映してよい(ApplyEffectiveConfigToUi() はデバイスコンボへ触れない)。
	ApplyEffectiveConfigToUi();
	UpdateStateDisplay();

	connect(&state_timer_, &QTimer::timeout, this, &ExternalOutputDock::OnStatePollTimer);
	connect(&meter_timer_, &QTimer::timeout, this, &ExternalOutputDock::OnMeterPollTimer);
	connect(&monitor_only_timer_, &QTimer::timeout, this, &ExternalOutputDock::OnMonitorOnlyPollTimer);
	restart_debounce_timer_.setSingleShot(true);
	connect(&restart_debounce_timer_, &QTimer::timeout, this, &ExternalOutputDock::OnRestartDebounceTimeout);

	// ポーリング間隔は slice-5.md の実装内容どおり: 状態 500ms、メーター 33ms(≒30Hz)、
	// MONITOR_ONLY 2s。EXIT で OnExit() が停止する(qt-ui-and-lifecycle.md)。
	// レビュー指摘 Medium-1: meter_timer_ はここでは開始しない。Running 状態のときだけ動かす
	// (UpdateStateDisplay() 参照)。非 Running の間は 30Hz ポーリングを止め、アイドル時のプラグイン
	// 消費を control thread(+この state_timer_ の 500ms ポーリング)へ集約する
	// (tools/measure/README.md のアイドル測定注記参照)。
	state_timer_.start(500);
	monitor_only_timer_.start(2000);

	// レビュー指摘 H-2: 初回のデバイス列挙(AudioDeviceManager::Enumerate()、COM 呼び出しを伴う)は
	// obs_module_load() を重くしないよう次のイベントループへ遅延する(この Dock は obs_module_load()
	// から直接構築される)。構造的な非同期化(control thread への移動)は S6 で行う。ここでは
	// コンストラクタを COM 呼び出しでブロックしないだけの軽減策。GetEffectiveConfig() の読み込みは
	// OnInitialDeviceEnumeration() 側で行う(その時点の最新値を使うため)。
	QTimer::singleShot(0, this, &ExternalOutputDock::OnInitialDeviceEnumeration);
}

ExternalOutputDock::~ExternalOutputDock() = default;

void ExternalOutputDock::OnExit()
{
	state_timer_.stop();
	meter_timer_.stop();
	monitor_only_timer_.stop();
	restart_debounce_timer_.stop();

	// Slice 9(REQ-COMPAT-001 制約2): 互換モード有効中なら EXIT で必ずランタイム復元する(元の
	// モニタリングデバイス・各ソースの monitoring type)。Disable() は例外を出さず淡々とログのみで
	// 進む設計(slice-9.md 実装内容の要点2)。
	// レビュー指摘(High-B、S9-10): OBS は EXIT イベントより前に SaveProjectNow() 済みのため、ここでの
	// obs_source_set_monitoring_type 変更はシーンコレクションに保存されない。compat-restore.json を
	// 削除すると次回起動時の唯一の永続化される復元経路(RestoreStaleFileIfPresent())を失うため、
	// is_exit_context=true でファイル削除を禁止する。
	if (compat_controller_.IsEnabled())
		compat_controller_.Disable(/*is_exit_context=*/true);

	// レビュー指摘 M-8: EXIT 後は入力を受け付けない。setEnabled(false) でウィジェットへの新規入力を
	// 止め、exiting_ フラグでキュー済みのシグナルが後から配送されても no-op にする(二重に呼んでも
	// 安全: setEnabled(false)/exiting_=true はいずれも冪等)。
	exiting_ = true;
	setEnabled(false);
}

void ExternalOutputDock::BuildUi()
{
	// スクロール対応(2026-09-01、スクリーンショット検証: Dock の縦スペースが不足すると詳細設定
	// パネルが Dock 領域の外(OBS ステータスバー上)にはみ出して描画される不具合の修正)。
	// ExternalOutputDock(this)直下は margin 0 の QVBoxLayout + QScrollArea のみとし、既存の全
	// コンテンツは QScrollArea の内容ウィジェット(content_widget)側へ移す。横スクロールバーは
	// 常時 Off にし、内容は viewport 幅に追従して折り返す(既存の Expanding ポリシー/
	// ApplyWordWrapSizePolicy の heightForWidth 計算がそのまま担保する。変更なし)。縦だけ AsNeeded で
	// スクロール可能にすることで、Dock 高さ不足時に内容が Dock 領域の外へはみ出す代わりに縦スクロール
	// バーが出るようにする。
	auto *outer_layout = new QVBoxLayout(this);
	outer_layout->setContentsMargins(0, 0, 0, 0);

	auto *scroll_area = new QScrollArea(this);
	scroll_area->setWidgetResizable(true);
	scroll_area->setFrameShape(QFrame::NoFrame);
	scroll_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll_area->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	outer_layout->addWidget(scroll_area);
	setLayout(outer_layout);

	auto *content_widget = new QWidget(scroll_area);
	content_widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

	auto *root_layout = new QVBoxLayout(content_widget);

	// 出力先(REQUIREMENTS.md §5)。
	auto *device_row = new QHBoxLayout();
	device_row->addWidget(new QLabel(Tr("ExternalAudioOutput.Dock.OutputDevice"), this));
	device_combo_ = new DeviceComboBox(this);
	// レイアウト再設計(2026-08-31、実機フィードバック): Dock を最小幅にするとコンボのテキストが
	// 潰れる問題への対応。Expanding + setMinimumContentsLength() で、狭い幅でも省略記号(…)により
	// 折り返さず 1 行で elide される状態を保つ。
	device_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	device_combo_->setMinimumContentsLength(8);
	device_row->addWidget(device_combo_, 1);
	refresh_devices_button_ = new QToolButton(this);
	refresh_devices_button_->setText(QStringLiteral("\xE2\x86\xBB")); // ↻(記号自体は locale 対象外)
	refresh_devices_button_->setToolTip(Tr("ExternalAudioOutput.Dock.RefreshDevices"));
	device_row->addWidget(refresh_devices_button_);
	root_layout->addLayout(device_row);

	// 状態表示。
	state_label_ = new QLabel(this);
	root_layout->addWidget(state_label_);

	// Level Meter。
	level_meter_ = new LevelMeterWidget(this);
	root_layout->addWidget(level_meter_);

	// 開始/停止トグル。
	start_stop_button_ = new QPushButton(this);
	root_layout->addWidget(start_stop_button_);

	// MONITOR_ONLY 警告(REQ-UI-007、既定非表示。Slice 9: 互換モード中は文言を差し替える)。
	monitor_only_warning_label_ = new QLabel(this);
	monitor_only_warning_label_->setWordWrap(true);
	// レイアウト再設計(2026-08-31、実機フィードバック追加分): wordWrap(true) のラベルで横方向を
	// Ignored にすると、QFormLayout/QVBoxLayout の heightForWidth 計算からこのラベルの sizeHint が
	// 外れてしまい、折り返し後の高さが正しく確保されず末尾がクリップする不具合があった。Preferred +
	// setMinimumWidth(1) にすることで、最小幅の押し上げ(改行不可能な語の幅が minimumSizeHint に
	// 入る問題)は引き続き防ぎつつ、heightForWidth による正しい折り返し高さを確保する。
	ApplyWordWrapSizePolicy(monitor_only_warning_label_, QSizePolicy::Preferred, QSizePolicy::Preferred);
	monitor_only_warning_label_->setMinimumWidth(1);
	monitor_only_warning_label_->setStyleSheet(QStringLiteral("color: #d08a00;"));
	monitor_only_warning_label_->setVisible(false);
	root_layout->addWidget(monitor_only_warning_label_);

	// Slice 9(03-compat-mode-reassessment.md 必須制約5、事実5 DO_NOT_SELF_MONITOR): 自己無音警告
	// (既定非表示)。REQ-COMPAT-001 自体の 5 項制約(正本 REQUIREMENTS.md)には含まれない別出典のため、
	// 番号を混同しないよう出典を書き分ける(REQ-COMPAT-001 制約5 は排他)。
	compat_self_silence_warning_label_ = new QLabel(this);
	compat_self_silence_warning_label_->setWordWrap(true);
	// クリップ修正(2026-08-31、実機フィードバック追加分): monitor_only_warning_label_ と同じ理由で
	// Ignored → Preferred + setMinimumWidth(1)。
	ApplyWordWrapSizePolicy(compat_self_silence_warning_label_, QSizePolicy::Preferred, QSizePolicy::Preferred);
	compat_self_silence_warning_label_->setMinimumWidth(1);
	compat_self_silence_warning_label_->setStyleSheet(QStringLiteral("color: #d08a00;"));
	compat_self_silence_warning_label_->setVisible(false);
	root_layout->addWidget(compat_self_silence_warning_label_);

	// Slice 9(REQ-COMPAT-001 制約4): 互換モード有効中の常時警告(既定非表示)。
	compat_mode_warning_label_ = new QLabel(Tr("ExternalAudioOutput.Dock.CompatModeWarning"), this);
	compat_mode_warning_label_->setWordWrap(true);
	// クリップ修正(2026-08-31、実機フィードバック追加分): 同上の理由で Ignored → Preferred +
	// setMinimumWidth(1)。
	ApplyWordWrapSizePolicy(compat_mode_warning_label_, QSizePolicy::Preferred, QSizePolicy::Preferred);
	compat_mode_warning_label_->setMinimumWidth(1);
	compat_mode_warning_label_->setStyleSheet(QStringLiteral("color: #d08a00; font-weight: bold;"));
	compat_mode_warning_label_->setVisible(false);
	root_layout->addWidget(compat_mode_warning_label_);

	// レビュー指摘(Low-3): 互換モード有効中、既にモニタリング設定済みの他ソースの音も鳴る旨の注記
	// (常時警告ラベルの下、小さめの文字、既定非表示)。
	compat_other_sources_note_label_ = new QLabel(Tr("ExternalAudioOutput.Dock.Compat.OtherSourcesNote"), this);
	compat_other_sources_note_label_->setWordWrap(true);
	// クリップ修正(2026-08-31、実機フィードバック追加分): 同上の理由で Ignored → Preferred +
	// setMinimumWidth(1)。
	ApplyWordWrapSizePolicy(compat_other_sources_note_label_, QSizePolicy::Preferred, QSizePolicy::Preferred);
	compat_other_sources_note_label_->setMinimumWidth(1);
	QFont other_sources_note_font = compat_other_sources_note_label_->font();
	other_sources_note_font.setPointSizeF(other_sources_note_font.pointSizeF() * 0.9);
	compat_other_sources_note_label_->setFont(other_sources_note_font);
	compat_other_sources_note_label_->setVisible(false);
	root_layout->addWidget(compat_other_sources_note_label_);

	// オンボーディングヒント(常時表示、小さめの文字)。
	onboarding_hint_label_ = new QLabel(this);
	onboarding_hint_label_->setWordWrap(true);
	// クリップ修正(2026-08-31、実機フィードバック追加分): 同上の理由で Ignored → Preferred +
	// setMinimumWidth(1)。
	ApplyWordWrapSizePolicy(onboarding_hint_label_, QSizePolicy::Preferred, QSizePolicy::Preferred);
	onboarding_hint_label_->setMinimumWidth(1);
	QFont hint_font = onboarding_hint_label_->font();
	hint_font.setPointSizeF(hint_font.pointSizeF() * 0.9);
	onboarding_hint_label_->setFont(hint_font);
	root_layout->addWidget(onboarding_hint_label_);

	// 詳細設定(折りたたみ、既定閉、REQUIREMENTS.md §5)。
	advanced_toggle_ = new QToolButton(this);
	advanced_toggle_->setText(Tr("ExternalAudioOutput.Dock.Advanced.Title"));
	advanced_toggle_->setCheckable(true);
	advanced_toggle_->setChecked(false);
	advanced_toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	advanced_toggle_->setArrowType(Qt::RightArrow);
	root_layout->addWidget(advanced_toggle_);

	// レイアウト再設計(2026-08-31、実機フィードバック): 最小幅にするとバッファのコンボが崩れる
	// (テキスト潰れ)問題への対応。QFormLayout の 2 カラム(ラベル列は sizeHint 固定、フィールド列のみ
	// 伸縮、ExpandingFieldsGrow)へ再構成し、並び順を REQUIREMENTS.md §5 の表の並びに揃える:
	// ①音声トラック ②出力方式(+説明) ③バッファ ④チャンク(custom 時のみ表示) ⑤自動再接続
	// ⑥AutoSaveNote+診断コピー(最下行)。
	advanced_panel_ = new QWidget(this);
	advanced_form_layout_ = new QFormLayout(advanced_panel_);
	advanced_form_layout_->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	advanced_form_layout_->setRowWrapPolicy(QFormLayout::DontWrapRows);

	// ① 音声トラック(スピン+tooltip)。
	audio_track_spin_ = new QSpinBox(advanced_panel_);
	audio_track_spin_->setRange(audio_track_map::kMinAudioTrack, audio_track_map::kMaxAudioTrack);
	// UX 改定(2026-08-31): 通常は変更不要である旨の tooltip(REQUIREMENTS.md §5)。
	audio_track_spin_->setToolTip(Tr("ExternalAudioOutput.Dock.Advanced.AudioTrackTooltip"));
	advanced_form_layout_->addRow(Tr("ExternalAudioOutput.Dock.Advanced.AudioTrack"), audio_track_spin_);

	// ② 出力方式(素の addRow。バッファ行(③)と同型式に統一)。
	// Slice 9(REQ-COMPAT-001): 出力方式(独立出力/OBS モニタリング互換、REQUIREMENTS.md §5)。
	// UX 改定(2026-08-31): 各アイテムに tooltip を追加し、選択中モードの説明をコンボ直下に表示する。
	// レイアウト再修正(2026-08-31、実機フィードバック第2弾): QFormLayout の行フィールドに
	// QWidget コンテナ(QVBoxLayout: コンボ+説明)を差し込む構成は、QFormLayout が
	// コンテナ入り行の heightForWidth を正しく計算できず行高が潰れ、コンボ自体が縦に潰れて
	// 見切れる不具合を引き起こした。正常描画できているバッファ行(③)と同じ「素の
	// addRow(ラベル, コンボ)」に戻し、コンボに余計な親コンテナを挟まない。
	output_mode_combo_ = new QComboBox(advanced_panel_);
	output_mode_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	output_mode_combo_->setMinimumContentsLength(8);
	output_mode_combo_->addItem(Tr("ExternalAudioOutput.Dock.Advanced.OutputModeIndependent"));
	output_mode_combo_->setItemData(0, Tr("ExternalAudioOutput.Dock.Advanced.OutputModeDescIndependent"),
					Qt::ToolTipRole);
	output_mode_combo_->addItem(Tr("ExternalAudioOutput.Dock.Advanced.OutputModeCompat"));
	output_mode_combo_->setItemData(1, Tr("ExternalAudioOutput.Dock.Advanced.OutputModeDescCompat"),
					Qt::ToolTipRole);
	advanced_form_layout_->addRow(Tr("ExternalAudioOutput.Dock.Advanced.OutputMode"), output_mode_combo_);

	// ②' 出力方式の説明ラベル。独立したフォーム行として追加する(ラベル列は空文字の QLabel)。
	// フィールド列に揃うため見た目はコンボ直下・同じ左端に並び、上のコンボに従属する注記であることが
	// 視覚的に分かる。コンテナに包まないため QFormLayout の heightForWidth 計算を壊さない。
	output_mode_desc_label_ = new QLabel(advanced_panel_);
	output_mode_desc_label_->setWordWrap(true);
	// クリップ修正(2026-08-31、実機フィードバック追加分→再修正): このラベルは QFormLayout の
	// field 列に置かれているため、他の折返しラベル(フォーム外、QVBoxLayout 配下)と異なり
	// 横 Preferred のままだと ExpandingFieldsGrow でも field 列幅まで広がらず、幅ほぼゼロに潰れて
	// 折り返される。横は Expanding にして field 列幅へ広げ、縦は Preferred のまま
	// heightForWidth による折り返し高さ確保を維持する。setMinimumWidth(1) は改行不可能な語の幅が
	// minimumSizeHint に入る問題を防ぐため引き続き必要。
	ApplyWordWrapSizePolicy(output_mode_desc_label_, QSizePolicy::Expanding, QSizePolicy::Preferred);
	output_mode_desc_label_->setMinimumWidth(1);
	// 左に軽いインデント(2〜4px)を付け、コンボに従属する注記であることを示す。
	output_mode_desc_label_->setContentsMargins(4, 0, 0, 0);
	QFont output_mode_desc_font = output_mode_desc_label_->font();
	output_mode_desc_font.setPointSizeF(output_mode_desc_font.pointSizeF() * 0.85);
	output_mode_desc_label_->setFont(output_mode_desc_font);
	// 淡色化(実機フィードバック追加分): ハードコード色ではなく、現在の QPalette の
	// Disabled/WindowText(プレースホルダー的な淡色、ライト/ダークテーマ双方に追従)を通常状態の
	// 文字色として流用する。
	QPalette output_mode_desc_palette = output_mode_desc_label_->palette();
	output_mode_desc_palette.setColor(QPalette::WindowText,
					  output_mode_desc_palette.color(QPalette::Disabled, QPalette::WindowText));
	output_mode_desc_label_->setPalette(output_mode_desc_palette);
	advanced_form_layout_->addRow(new QLabel(QString(), advanced_panel_), output_mode_desc_label_);

	// ③ バッファ(コンボのみ。チャンク行は④として分離)。
	// UX 改定(2026-08-31): ラジオ 4 択 → QComboBox に置換(出力方式・出力先と同型式、
	// REQUIREMENTS.md §5)。既存の locale キー(BufferPresetAuto 等)をアイテムラベルに流用する。
	buffer_preset_combo_ = new QComboBox(advanced_panel_);
	buffer_preset_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	buffer_preset_combo_->setMinimumContentsLength(8);
	buffer_preset_combo_->addItem(Tr("ExternalAudioOutput.Dock.Advanced.BufferPresetAuto"), QStringLiteral("auto"));
	buffer_preset_combo_->addItem(Tr("ExternalAudioOutput.Dock.Advanced.BufferPresetLowLatency"),
				      QStringLiteral("low_latency"));
	buffer_preset_combo_->addItem(Tr("ExternalAudioOutput.Dock.Advanced.BufferPresetStable"),
				      QStringLiteral("stable"));
	buffer_preset_combo_->addItem(Tr("ExternalAudioOutput.Dock.Advanced.BufferPresetCustom"),
				      QStringLiteral("custom"));
	advanced_form_layout_->addRow(Tr("ExternalAudioOutput.Dock.Advanced.BufferPreset"), buffer_preset_combo_);

	// ④ チャンク行(スライダー+「%1 チャンク」値ラベル)。実機フィードバック: 自動プリセット選択時にも
	// 常時表示されて分かりづらかったため、buffer_preset_combo_=="custom" のときだけ setRowVisible() で
	// 行ごと表示する(UpdateBufferChunksRowVisibility() が切替のたびに呼ぶ。disable ではなく非表示に
	// することで縦方向も詰まる)。
	buffer_chunks_row_widget_ = new QWidget(advanced_panel_);
	auto *buffer_chunks_row = new QHBoxLayout(buffer_chunks_row_widget_);
	buffer_chunks_row->setContentsMargins(0, 0, 0, 0);
	buffer_chunks_slider_ = new QSlider(Qt::Horizontal, buffer_chunks_row_widget_);
	buffer_chunks_slider_->setRange(2, 16);
	buffer_chunks_value_label_ = new QLabel(buffer_chunks_row_widget_);
	buffer_chunks_row->addWidget(buffer_chunks_slider_, 1);
	buffer_chunks_row->addWidget(buffer_chunks_value_label_);
	advanced_form_layout_->addRow(buffer_chunks_row_widget_);

	// ⑤ 自動再接続(チェック+注記)。注記は既存どおり tooltip(AutoReconnectNote)。REQUIREMENTS.md §5 は
	// チェックボックスのみを規定しており、常時表示テキストは追加しない(locale キー・文言は不変)。
	auto_reconnect_check_ = new QCheckBox(Tr("ExternalAudioOutput.Dock.Advanced.AutoReconnect"), advanced_panel_);
	auto_reconnect_check_->setToolTip(Tr("ExternalAudioOutput.Dock.Advanced.AutoReconnectNote"));
	advanced_form_layout_->addRow(auto_reconnect_check_);

	// UX 改定(2026-08-31): ログレベル UI は削除(config.json の手編集でのみ変更可能、
	// REQUIREMENTS.md §5)。適用ロジック(plugin-config / OutputInstance::ApplyLogLevelThreshold())は
	// UI に依存しておらず不変。

	// ⑥ 最下行: AutoSaveNote(左、小さめフォント)+ 診断コピー(右端、アイコンのみの小型ボタン)。
	auto *bottom_row_widget = new QWidget(advanced_panel_);
	auto *bottom_row_layout = new QHBoxLayout(bottom_row_widget);
	bottom_row_layout->setContentsMargins(0, 0, 0, 0);

	// Slice 8: locale キーを VolatileNote → AutoSaveNote に改名(「保存されない」ではなく「自動保存
	// される」意味であり、旧名は意味が逆に取れかねなかったため)。
	auto *auto_save_note_label =
		new QLabel(Tr("ExternalAudioOutput.Dock.Advanced.AutoSaveNote"), bottom_row_widget);
	auto_save_note_label->setWordWrap(true);
	// クリップ修正(2026-08-31、実機フィードバック追加分): 同上の理由で Ignored → Preferred +
	// setMinimumWidth(1)。
	ApplyWordWrapSizePolicy(auto_save_note_label, QSizePolicy::Preferred, QSizePolicy::Preferred);
	auto_save_note_label->setMinimumWidth(1);
	QFont note_font = auto_save_note_label->font();
	note_font.setPointSizeF(note_font.pointSizeF() * 0.85);
	auto_save_note_label->setFont(note_font);
	bottom_row_layout->addWidget(auto_save_note_label, 1);

	// UX 改定(2026-08-31)/実機フィードバック(レイアウト再設計): 診断情報コピーは常時表示エリアから
	// 詳細設定内(最下部)へ既に移動済み。今回さらに、大きすぎた QPushButton をアイコンのみの小型
	// QToolButton(約 22x22px、autoRaise)へ変更する(機能・locale キー CopyDiagnostics は不変、AC-25)。
	copy_diagnostics_button_ = new QToolButton(bottom_row_widget);
	copy_diagnostics_button_->setAutoRaise(true);
	copy_diagnostics_button_->setFixedSize(22, 22);
	QIcon copy_diagnostics_icon = QIcon::fromTheme(QStringLiteral("edit-copy"));
	if (copy_diagnostics_icon.isNull())
		copy_diagnostics_icon = style()->standardIcon(QStyle::SP_FileDialogDetailedView);
	if (!copy_diagnostics_icon.isNull()) {
		copy_diagnostics_button_->setIcon(copy_diagnostics_icon);
	} else {
		// テーマアイコン・標準アイコンのいずれも取得できない環境向けの最終フォールバック
		// (「⧉」記号自体は locale 対象外)。
		copy_diagnostics_button_->setText(QStringLiteral("\xE2\xA7\x89")); // ⧉
	}
	copy_diagnostics_button_->setToolTip(Tr("ExternalAudioOutput.Dock.CopyDiagnostics"));
	copy_diagnostics_button_->setAccessibleName(Tr("ExternalAudioOutput.Dock.CopyDiagnostics"));
	bottom_row_layout->addWidget(copy_diagnostics_button_);

	advanced_form_layout_->addRow(bottom_row_widget);

	advanced_panel_->setLayout(advanced_form_layout_);
	advanced_panel_->setVisible(false); // 既定閉(REQUIREMENTS.md §5)。
	root_layout->addWidget(advanced_panel_);

	root_layout->addStretch(1);
	content_widget->setLayout(root_layout);
	scroll_area->setWidget(content_widget);

	// レイアウト再設計(2026-08-31、実機フィードバック): Dock を最小幅にするとコンボのテキストが
	// 潰れる問題への対応。詳細設定の最長コンボアイテム(出力方式「OBS モニタリング互換」)+ ラベル列が
	// elide せず収まる程度を Dock コンテンツの最小幅として確保する(フォントメトリクスからの概算を
	// 240〜280px にクランプ。正確な値は実機のフォント・DPI 依存のため実測で調整可能)。
	// スクロール対応(2026-09-01): この最小幅は内容ウィジェット(content_widget)側に設定する。
	// QScrollArea 自体にも「内容最小幅 + 縦スクロールバー幅」を最小幅として設定し、縦スクロールバーが
	// 表示されて viewport 幅が削られたときでも、横スクロールバー常時 Off のまま内容が横クリップされない
	// ようにする(PM_ScrollBarExtent は縦スクロールバー未表示時でも取得できる標準サイズ指標)。
	{
		const QFontMetrics dock_font_metrics(font());
		const int longest_item_width =
			dock_font_metrics.horizontalAdvance(Tr("ExternalAudioOutput.Dock.Advanced.OutputModeCompat"));
		const int label_column_width =
			dock_font_metrics.horizontalAdvance(Tr("ExternalAudioOutput.Dock.Advanced.OutputMode"));
		int min_width = longest_item_width + label_column_width + 80; // コンボの矢印・余白・行間の余裕分
		if (min_width < 240)
			min_width = 240;
		if (min_width > 280)
			min_width = 280;
		content_widget->setMinimumWidth(min_width);
		const int scrollbar_extent = style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this);
		scroll_area->setMinimumWidth(min_width + scrollbar_extent);
	}

	connect(device_combo_, &DeviceComboBox::AboutToOpenPopup, this, &ExternalOutputDock::OnDeviceComboAboutToOpen);
	connect(device_combo_, &QComboBox::activated, this, &ExternalOutputDock::OnDeviceComboActivated);
	connect(refresh_devices_button_, &QToolButton::clicked, this, &ExternalOutputDock::OnRefreshDevicesClicked);
	connect(start_stop_button_, &QPushButton::clicked, this, &ExternalOutputDock::OnStartStopClicked);
	connect(copy_diagnostics_button_, &QToolButton::clicked, this, &ExternalOutputDock::OnCopyDiagnosticsClicked);
	connect(advanced_toggle_, &QToolButton::toggled, this, &ExternalOutputDock::OnAdvancedToggled);
	// QSpinBox::valueChanged/QComboBox::currentIndexChanged はオーバーロード(int 版と
	// 非推奨の QString 版)を持つため、QOverload<int>::of() で int 版を明示する(曖昧呼び出しの
	// コンパイルエラー回避)。
	connect(audio_track_spin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
		&ExternalOutputDock::OnAudioTrackChanged);
	// レビュー指摘 H-1: 実効値への反映(Restart())はフォーカスアウト/Enter(editingFinished)を
	// デバウンスして行う。valueChanged は表示(オンボーディングヒント)の即時更新にのみ使う。
	connect(audio_track_spin_, &QSpinBox::editingFinished, this, &ExternalOutputDock::OnAudioTrackEditingFinished);
	// UX 改定(2026-08-31): ラジオ(idClicked)→ QComboBox(currentIndexChanged)。
	connect(buffer_preset_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&ExternalOutputDock::OnBufferPresetChanged);
	connect(buffer_chunks_slider_, &QSlider::valueChanged, this, &ExternalOutputDock::OnBufferChunksSliderChanged);
	// レビュー指摘 H-1: 実効値への反映(Restart())はドラッグ終了(sliderReleased)をデバウンスして
	// 行う。valueChanged はドラッグ中の表示ラベル更新にのみ使う。
	connect(buffer_chunks_slider_, &QSlider::sliderReleased, this,
		&ExternalOutputDock::OnBufferChunksSliderReleased);
	connect(auto_reconnect_check_, &QCheckBox::toggled, this, &ExternalOutputDock::OnAutoReconnectToggled);
	connect(output_mode_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&ExternalOutputDock::OnOutputModeChanged);
}

void ExternalOutputDock::RefreshDeviceList(const QString &prefer_device_id, const QString &prefer_device_name)
{
	// 選択対象の device_id/表示名を決める: 呼び出し元が明示した prefer_device_id があればそれを
	// 優先する(初回列挙・config.json 復元用)。無ければ、現在コンボが選択している device_id を
	// 可能な限り保持する(ポップアップ再列挙/更新ボタン用。REQUIREMENTS.md §5)。
	QString target_id = prefer_device_id;
	QString target_name = prefer_device_name;
	if (target_id.isEmpty() && device_combo_->count() > 0) {
		const int current = device_combo_->currentIndex();
		target_id = device_combo_->itemData(current).toString();
		target_name = device_combo_->itemText(current);
	}

	device_combo_populating_ = true;
	device_combo_->clear();
	device_combo_->addItem(Tr("ExternalAudioOutput.Dock.DefaultDeviceOption"), QStringLiteral("default"));

	// AudioDeviceManager::Enumerate() は Qt UI スレッド(STA)から安全に呼べる(wasapi-com.md、
	// 既存の Tools メニュー「List Devices (Debug)」と同じ経路)。Dock は WASAPI/COM を直接触らない。
	const AudioDeviceEnumeration enumeration = AudioDeviceManager::Enumerate();
	if (!enumeration.succeeded) {
		obs_log(LOG_WARNING,
			"ExternalOutputDock: device enumeration failed (hr=0x%08lX) while refreshing the device "
			"list",
			(unsigned long)enumeration.enumeration_hr);
	} else {
		for (const auto &device : enumeration.devices)
			device_combo_->addItem(QString::fromUtf8(device.name.c_str()),
					       QString::fromUtf8(device.id.c_str()));
	}

	int restore_index = 0;
	if (!target_id.isEmpty()) {
		const int found = device_combo_->findData(target_id);
		if (found >= 0) {
			restore_index = found;
		} else {
			// レビュー指摘 M-5: 選択中/保存された device_id が今回の列挙に無い(未接続、または
			// まだ列挙が完了していない初回呼び出し)。黙って既定デバイス表示へ戻さず、
			// プレースホルダー項目を追加して選択状態にする(次回の列挙で実デバイスが見つかれば、
			// この device_id を itemData に持つ項目として自動的に置き換わる)。
			const QString label_source = target_name.isEmpty() ? target_id : target_name;
			device_combo_->addItem(Tr("ExternalAudioOutput.Dock.UnknownDeviceOption").arg(label_source),
					       target_id);
			restore_index = device_combo_->count() - 1;
		}
	}
	device_combo_->setCurrentIndex(restore_index);
	device_combo_populating_ = false;
}

void ExternalOutputDock::ApplyEffectiveConfigToUi()
{
	const output_instance::ConfigValues config = output_instance_->GetEffectiveConfig();

	// デバイスコンボの選択は RefreshDeviceList() の責務(レビュー指摘 H-2/M-5: 初回は列挙前に
	// このメソッドが呼ばれるため、ここでは触らない)。

	audio_track_spin_->blockSignals(true);
	audio_track_spin_->setValue(config.audio_track);
	audio_track_spin_->blockSignals(false);

	int buffer_preset_index = 0; // "auto" が既定(REQUIREMENTS.md §3)。
	if (config.buffer_preset == "low_latency")
		buffer_preset_index = 1;
	else if (config.buffer_preset == "stable")
		buffer_preset_index = 2;
	else if (config.buffer_preset == "custom")
		buffer_preset_index = 3;
	buffer_preset_combo_->blockSignals(true);
	buffer_preset_combo_->setCurrentIndex(buffer_preset_index);
	buffer_preset_combo_->blockSignals(false);

	buffer_chunks_slider_->blockSignals(true);
	buffer_chunks_slider_->setValue((int)config.buffer_chunks);
	buffer_chunks_slider_->blockSignals(false);
	buffer_chunks_value_label_->setText(
		Tr("ExternalAudioOutput.Dock.Advanced.BufferChunksValue").arg((int)config.buffer_chunks));
	// レイアウト再設計(2026-08-31、実機フィードバック): チャンク行は buffer_preset=="custom" のときだけ
	// 行ごと表示する(setRowVisible、disable ではなく非表示。UpdateBufferChunksRowVisibility() 参照)。
	UpdateBufferChunksRowVisibility();

	auto_reconnect_check_->blockSignals(true);
	auto_reconnect_check_->setChecked(config.auto_reconnect);
	auto_reconnect_check_->blockSignals(false);

	// UX 改定(2026-08-31): ログレベル UI は削除済み。config.json の log_level は
	// OutputInstance::ApplyLogLevelThreshold() が Restart()/起動時に適用するのみで、この Dock の UI へは
	// 反映しない(REQUIREMENTS.md §5)。

	// Slice 9(REQ-COMPAT-001): output_mode は ConfigValues に含まれない(独立出力の実行時パラメータ
	// ではなく、独立出力/互換モードの排他選択そのものであるため)。plugin_config::Load() から直接読む。
	const plugin_config::PluginConfig persisted = plugin_config::Load();
	output_mode_combo_->blockSignals(true);
	output_mode_combo_->setCurrentIndex(persisted.output_mode == "obs_monitor_compat" ? 1 : 0);
	output_mode_combo_->blockSignals(false);
	UpdateOutputModeDescription();

	UpdateOnboardingHint();
}

bool ExternalOutputDock::IsCompatModeSelected() const
{
	return output_mode_combo_->currentIndex() == 1;
}

void ExternalOutputDock::UpdateOutputModeDescription()
{
	// UX 改定(2026-08-31): 選択中モードの説明を 1 行表示する(REQUIREMENTS.md §5「出力方式の説明」)。
	output_mode_desc_label_->setText(Tr(IsCompatModeSelected()
						    ? "ExternalAudioOutput.Dock.Advanced.OutputModeDescCompat"
						    : "ExternalAudioOutput.Dock.Advanced.OutputModeDescIndependent"));
}

void ExternalOutputDock::UpdateBufferChunksRowVisibility()
{
	// レイアウト再設計(2026-08-31、実機フィードバック): buffer_preset_combo_ が "custom" のときだけ
	// チャンク行(スライダー+値ラベル)を表示する。setRowVisible() は行(ラベル列含む)ごと隠すため、
	// disable するだけの旧実装と異なり非表示時は縦方向にも詰まる。
	const bool is_custom = buffer_preset_combo_->currentData().toString() == QStringLiteral("custom");
	advanced_form_layout_->setRowVisible(buffer_chunks_row_widget_, is_custom);
}

void ExternalOutputDock::ResetMeterToFloor()
{
	if (!meter_timer_.isActive())
		return;
	meter_timer_.stop();
	meter_display_l_db_ = kMeterInternalFloorDb;
	meter_display_r_db_ = kMeterInternalFloorDb;
	meter_peak_hold_l_db_ = kMeterInternalFloorDb;
	meter_peak_hold_r_db_ = kMeterInternalFloorDb;
	meter_peak_hold_l_remaining_ms_ = 0;
	meter_peak_hold_r_remaining_ms_ = 0;
	level_meter_->SetLevels(meter_display_l_db_, meter_display_r_db_, meter_peak_hold_l_db_, meter_peak_hold_r_db_);
}

void ExternalOutputDock::UpdateCompatStateDisplay()
{
	// Slice 9(REQ-COMPAT-001): 互換モード中は独立出力(OutputInstance)を起動しないため
	// (output-instance.cpp の HandleStartCommand() ガード)、GetState() は通常 Stopped のまま。
	// レビュー指摘(High-3): MonitorCompatController::Enable() の停止待ちタイムアウト直後など、
	// 何らかの理由で独立出力が Stopped でない場合は「見えず止められない出力」を防ぐため、通常の
	// compat 表示より優先して異常表示にし、Stop() を直接呼べるボタンにする
	// (OnStartStopClicked() 参照)。
	const output_instance::State independent_state = output_instance_->GetState();
	if (independent_state != output_instance::State::Stopped) {
		state_label_->setText(QString::fromUtf8("\xE2\x9A\xA0 ") // ⚠
				      + Tr("ExternalAudioOutput.Dock.Compat.IndependentOutputAnomaly"));
		start_stop_button_->setText(Tr("ExternalAudioOutput.Dock.StopButton"));
		start_stop_button_->setEnabled(true);
		ResetMeterToFloor();
		level_meter_->setToolTip(Tr("ExternalAudioOutput.Dock.Compat.MeterDisabledNote"));
		device_combo_->setEnabled(true);
		refresh_devices_button_->setEnabled(true);
		advanced_panel_->setEnabled(true);
		compat_mode_warning_label_->setVisible(compat_controller_.IsEnabled());
		compat_other_sources_note_label_->setVisible(false);
		return;
	}

	// 表示は compat_controller_.IsEnabled() を真値として使う。
	const bool enabled = compat_controller_.IsEnabled();

	state_label_->setText(
		(enabled ? QString::fromUtf8("\xE2\x97\x8F ")  // ●
			 : QString::fromUtf8("\xE2\x97\x8B ")) // ○
		+ Tr(enabled ? "ExternalAudioOutput.Dock.State.Running" : "ExternalAudioOutput.Dock.State.Stopped"));
	start_stop_button_->setText(
		Tr(enabled ? "ExternalAudioOutput.Dock.StopButton" : "ExternalAudioOutput.Dock.StartButton"));
	start_stop_button_->setEnabled(true);

	// Level Meter は互換モードでは常に無効表示(独立出力が停止中のため。ARCHITECTURE.md §12 互換
	// モード追加事実)。レビュー指摘(Low-4): 専用の locale キーで理由を明示する(常時警告
	// CompatModeWarning とは意味が異なるため)。output_instance_ が Running へ遷移しないため
	// ResetMeterToFloor() は「既に止まっていれば何もしない」形で安全に毎回呼べる。
	ResetMeterToFloor();
	level_meter_->setToolTip(Tr("ExternalAudioOutput.Dock.Compat.MeterDisabledNote"));

	device_combo_->setEnabled(true);
	refresh_devices_button_->setEnabled(true);
	advanced_panel_->setEnabled(true);

	compat_mode_warning_label_->setVisible(enabled);
	// レビュー指摘(Low-3): 互換モード有効中、既にモニタリング設定済みの他ソースの音も鳴る旨の注記。
	compat_other_sources_note_label_->setVisible(enabled);
}

void ExternalOutputDock::UpdateStateDisplay()
{
	const output_instance::State state = output_instance_->GetState();
	// レビュー指摘(Medium-6): OnStartStopClicked() が参照する「直近に実際に表示した state」。
	last_displayed_state_ = state;

	if (IsCompatModeSelected()) {
		UpdateCompatStateDisplay();
		return;
	}
	compat_mode_warning_label_->setVisible(false);
	compat_other_sources_note_label_->setVisible(false);
	level_meter_->setToolTip(QString());

	const char *state_key = "ExternalAudioOutput.Dock.State.Stopped";
	const char *symbol = "\xE2\x97\x8B"; // ○
	const char *button_key = "ExternalAudioOutput.Dock.StartButton";

	switch (state) {
	case output_instance::State::Stopped:
		state_key = "ExternalAudioOutput.Dock.State.Stopped";
		symbol = "\xE2\x97\x8B"; // ○
		button_key = "ExternalAudioOutput.Dock.StartButton";
		break;
	case output_instance::State::Starting:
		state_key = "ExternalAudioOutput.Dock.State.Starting";
		symbol = "\xE2\x80\xA6"; // …
		// Slice 6: Start()/Stop() が非同期(enqueue のみ)になり Starting が実際に観測可能になった
		// ため、「キャンセル」ボタンを復活させる(押下で Stop コマンドを enqueue する)。
		button_key = "ExternalAudioOutput.Dock.CancelButton";
		break;
	case output_instance::State::Running:
		state_key = "ExternalAudioOutput.Dock.State.Running";
		symbol = "\xE2\x97\x8F"; // ●
		button_key = "ExternalAudioOutput.Dock.StopButton";
		break;
	case output_instance::State::Recovering:
		// Slice 6: 新設。再接続ループ中も「キャンセル」相当(Stop コマンド enqueue)で中断できる。
		state_key = "ExternalAudioOutput.Dock.State.Recovering";
		symbol = "\xE2\x80\xA6"; // …
		button_key = "ExternalAudioOutput.Dock.CancelButton";
		break;
	case output_instance::State::Failed:
		state_key = "ExternalAudioOutput.Dock.State.Failed";
		symbol = "\xE2\x9A\xA0"; // ⚠
		button_key = "ExternalAudioOutput.Dock.ReconnectButton";
		break;
	}

	state_label_->setText(QString::fromUtf8(symbol) + QStringLiteral(" ") + Tr(state_key));
	start_stop_button_->setText(Tr(button_key));

	// レビュー指摘 Medium-1: meter_timer_(30Hz)は Running 中だけ動かす。状態ポーリング(この関数、
	// 500ms)が Running を検出したら開始し、離れたら止めて表示を即座にフロアへ落とす(以後の
	// OnMeterPollTimer() による減衰アニメーションは行わない。タイマー自体を止めるため)。
	if (state == output_instance::State::Running) {
		if (!meter_timer_.isActive())
			meter_timer_.start(33);
	} else {
		ResetMeterToFloor();
	}

	// Slice 6: Start()/Stop()/Restart() が非同期化されたため、呼び出しごとに前後を無効化/復帰する
	// SetControlsEnabledDuringOperation() を廃止し、ポーリングした状態から常時反映する方式にした
	// (ブリーフ: 「setEnabled 制御は状態ポーリングで」)。Starting/Recovering 中は設定変更(デバイス
	// 選択・詳細設定)を無効化し、それ以外(Stopped/Running/Failed)では有効にする。開始/停止ボタン
	// 自体は Cancel/Reconnect 用途もあるため常に有効。
	const bool settings_enabled =
		(state != output_instance::State::Starting && state != output_instance::State::Recovering);
	device_combo_->setEnabled(settings_enabled);
	refresh_devices_button_->setEnabled(settings_enabled);
	advanced_panel_->setEnabled(settings_enabled);

	// レビュー指摘(Medium-6): 連打対策。OnStartStopClicked() がコマンドを送出した直後 1 秒間は
	// disable のままにし、それ以降のポーリングで通常どおり常時 enable へ戻す。
	bool start_stop_enabled = true;
	if (start_stop_command_pending_) {
		const auto elapsed = std::chrono::steady_clock::now() - start_stop_command_sent_at_;
		if (elapsed < std::chrono::seconds(1))
			start_stop_enabled = false;
		else
			start_stop_command_pending_ = false;
	}
	start_stop_button_->setEnabled(start_stop_enabled);
}

void ExternalOutputDock::UpdateOnboardingHint()
{
	onboarding_hint_label_->setText(Tr("ExternalAudioOutput.Dock.OnboardingHint").arg(audio_track_spin_->value()));
}

void ExternalOutputDock::UpdateMeterChannel(float new_db, float &display_db, float &peak_hold_db,
					    int &peak_hold_remaining_ms) const
{
	// D-15: UI 側で減衰 20dB/s とピークホールド 1s を適用する(値そのものは WasapiOutput の
	// per-wake atomic から読んだ瞬時ピーク)。33ms ポーリングを前提にした固定レートの減衰。
	constexpr float kDecayDbPerTick = 20.0f * (33.0f / 1000.0f);
	constexpr int kPeakHoldMs = 1000;
	constexpr int kIntervalMs = 33;

	if (new_db > display_db) {
		display_db = new_db;
	} else {
		display_db -= kDecayDbPerTick;
		if (display_db < kMeterInternalFloorDb)
			display_db = kMeterInternalFloorDb;
	}

	if (new_db >= peak_hold_db) {
		peak_hold_db = new_db;
		peak_hold_remaining_ms = kPeakHoldMs;
	} else if (peak_hold_remaining_ms > 0) {
		peak_hold_remaining_ms -= kIntervalMs;
	} else {
		peak_hold_db -= kDecayDbPerTick;
		if (peak_hold_db < kMeterInternalFloorDb)
			peak_hold_db = kMeterInternalFloorDb;
	}
}

void ExternalOutputDock::OnStatePollTimer()
{
	if (exiting_)
		return;
	UpdateStateDisplay();
}

void ExternalOutputDock::OnMeterPollTimer()
{
	if (exiting_)
		return;

	// レビュー指摘 M-3: 停止/Failed 中は WasapiOutput 側の値を読まず、常にフロア(無音)として扱う
	// (WasapiOutput::Stop() 側でも level_peak_* をリセットしているが、UI 側でも二重に保証する。
	// Stop() 後〜次の Start() までの間、backend_ の atomic には直近セッションの値が残り得るため)。
	const bool is_running = output_instance_->GetState() == output_instance::State::Running;
	// render thread(WasapiOutput::RenderLoop)が 1 wake に 1 回だけ store した atomic を読む
	// (D-15、rt-audio.md)。RT スレッドへは一切触れない。
	const float new_l_db = is_running ? LinearToDbfs(output_instance_->LevelPeakL()) : kMeterInternalFloorDb;
	const float new_r_db = is_running ? LinearToDbfs(output_instance_->LevelPeakR()) : kMeterInternalFloorDb;

	UpdateMeterChannel(new_l_db, meter_display_l_db_, meter_peak_hold_l_db_, meter_peak_hold_l_remaining_ms_);
	UpdateMeterChannel(new_r_db, meter_display_r_db_, meter_peak_hold_r_db_, meter_peak_hold_r_remaining_ms_);

	// レビュー指摘 L-13: 停止中に表示値が既にフロアまで落ち切っていれば、無音の再描画を毎 33ms
	// 繰り返さない。Medium-1(Slice 8 レビュー)以降、meter_timer_ 自体は Running を離れると
	// UpdateStateDisplay() が止めるため、is_running==false でこの関数が呼ばれるのは基本的に
	// Running→他状態への遷移が起きてから次の 500ms 状態ポーリングまでの間だけ(この分岐はその短い
	// 遷移窓での重複描画抑制として残す)。
	if (!is_running && meter_display_l_db_ <= kMeterInternalFloorDb &&
	    meter_display_r_db_ <= kMeterInternalFloorDb && meter_peak_hold_l_db_ <= kMeterInternalFloorDb &&
	    meter_peak_hold_r_db_ <= kMeterInternalFloorDb)
		return;

	level_meter_->SetLevels(meter_display_l_db_, meter_display_r_db_, meter_peak_hold_l_db_, meter_peak_hold_r_db_);
}

namespace {

// OnMonitorOnlyPollTimer()/UpdateMonitorWarnings() 共通: 名前一覧を「先頭 kMaxNamesShown 件 + ほかN件」
// のローカライズ済みテキストへ整形する(既存 MonitorOnlyWarning の書式を再利用)。
QString JoinSourceNames(const std::vector<std::string> &names)
{
	constexpr size_t kMaxNamesShown = 3;
	QStringList list;
	for (size_t i = 0; i < names.size() && i < kMaxNamesShown; ++i)
		list << QString::fromUtf8(names[i].c_str());
	QString text = list.join(QStringLiteral(", "));
	if (names.size() > kMaxNamesShown) {
		text += QStringLiteral(" ");
		text += Tr("ExternalAudioOutput.Dock.MonitorOnlyWarningMore").arg((int)(names.size() - kMaxNamesShown));
	}
	return text;
}

} // namespace

void ExternalOutputDock::UpdateMonitorWarnings()
{
	// REQ-UI-007/AC-24: 「選択中トラック」は Advanced 設定のスピナーが表示している値
	// (次回 Start()/Restart()/互換モード適用で使われる値と一致する)。
	const int audio_track = audio_track_spin_->value();

	// レビュー3回目指摘(Low-1): 互換モード有効中かつスピナーの値が直近の Reapply()/Enable() で
	// 適用済みのトラックと一致する場合は、compat_controller_.LastPlan().warn_monitor_only(同じ
	// obs_enum_sources() 結果から compat_plan::BuildEnablePlan() が分類済み)をそのまま使う。この
	// 2 秒 tick では compat_controller_.Reapply() が既に同じトラックを列挙しているため、
	// OutputInstance::FindMonitorOnlySources() で obs_enum_sources() を再度呼ばずに済む(tick あたりの
	// 列挙回数を 1 回に統合)。トラック未確定(スピナー編集中でまだ適用されていない)ときは従来どおり
	// 個別に列挙し直し、古いトラックの警告が誤表示されるのを避ける。
	// レビュー3回目残作業(Medium-3a): RollbackCompatModeToIndependent() が積んだ一回限りの警告が
	// あれば、通常の MONITOR_ONLY 判定より優先してこの呼び出しでだけ表示する(表示後は消費してクリア
	// するため、次回のこの関数呼び出し=次回状態変化で通常表示に戻る)。
	if (!pending_compat_rollback_warning_.isEmpty()) {
		monitor_only_warning_label_->setText(pending_compat_rollback_warning_);
		monitor_only_warning_label_->setVisible(true);
		pending_compat_rollback_warning_.clear();
	} else {
		std::vector<std::string> names;
		if (compat_controller_.IsEnabled() && compat_controller_.AudioTrack() == audio_track) {
			names = compat_controller_.LastPlan().warn_monitor_only;
		} else {
			const std::vector<output_instance::MonitorOnlySource> monitor_only_sources =
				OutputInstance::FindMonitorOnlySources(audio_track);
			names.reserve(monitor_only_sources.size());
			for (const auto &source : monitor_only_sources)
				names.push_back(source.name);
		}

		if (names.empty()) {
			monitor_only_warning_label_->setVisible(false);
		} else {
			const QString names_text = JoinSourceNames(names);

			// Slice 9(REQ-COMPAT-001 制約1): 互換モード中は「変更しない」ことを明示する専用文言にする。
			const QString label_text =
				compat_controller_.IsEnabled()
					? Tr("ExternalAudioOutput.Dock.Compat.MonitorOnlyUnchanged").arg(names_text)
					: Tr("ExternalAudioOutput.Dock.MonitorOnlyWarning").arg(names_text);
			monitor_only_warning_label_->setText(label_text);
			monitor_only_warning_label_->setVisible(true);
		}
	}

	// Slice 9(03-compat-mode-reassessment.md 必須制約5、事実5 DO_NOT_SELF_MONITOR): 自己無音警告。
	// 互換モード有効中のみ意味を持つ(REQ-COMPAT-001 自体の 5 項制約とは別出典、REQ-COMPAT-001 制約5
	// は排他)。
	const std::vector<std::string> &self_silence = compat_controller_.LastPlan().self_silence_risk;
	if (compat_controller_.IsEnabled() && !self_silence.empty()) {
		compat_self_silence_warning_label_->setText(
			Tr("ExternalAudioOutput.Dock.Compat.SelfSilenceWarning").arg(JoinSourceNames(self_silence)));
		compat_self_silence_warning_label_->setVisible(true);
	} else {
		compat_self_silence_warning_label_->setVisible(false);
	}
}

void ExternalOutputDock::OnMonitorOnlyPollTimer()
{
	if (exiting_)
		return;

	// Slice 9(REQ-COMPAT-001 制約3、実装内容の要点3): 互換モード有効中は既存の 2 秒タイマーで
	// 「OBS 側上書きの検知→再適用」と「新規ソースの追従」を行う。
	if (compat_controller_.IsEnabled()) {
		compat_controller_.Reapply();
		output_instance_->SetCompatEventCounts(compat_controller_.ApplyCount(),
						       compat_controller_.RestoreCount(),
						       compat_controller_.ReapplyCount());
	}

	UpdateMonitorWarnings();
}

void ExternalOutputDock::OnInitialDeviceEnumeration()
{
	if (exiting_)
		return;

	// レビュー指摘 H-2: コンストラクタで遅延した初回列挙。config.json 由来の device_id/device_name
	// を優先して選択する(RefreshDeviceList() 参照。見つからなければプレースホルダー表示。M-5)。
	const output_instance::ConfigValues config = output_instance_->GetEffectiveConfig();
	RefreshDeviceList(QString::fromUtf8(config.device_id.c_str()), QString::fromUtf8(config.device_name.c_str()));
}

void ExternalOutputDock::OnDeviceComboAboutToOpen()
{
	if (exiting_)
		return;
	RefreshDeviceList();
}

void ExternalOutputDock::OnDeviceComboActivated(int index)
{
	if (exiting_)
		return;
	if (device_combo_populating_)
		return;

	const QString device_id = device_combo_->itemData(index).toString();
	const QString device_name = device_combo_->itemText(index);

	output_instance::ConfigOverrides overrides;
	overrides.device_id = device_id.toStdString();
	// device_id=="default" のときは device_name を使わない(REQUIREMENTS.md §3)。
	overrides.device_name = device_id == QStringLiteral("default") ? std::string() : device_name.toStdString();

	// Slice 6: Restart() はコマンド enqueue のみで即座に return する(構造的な非同期化)。呼び出し前後の
	// 無効化ブラケットは廃止し、状態ポーリング(UpdateStateDisplay())側で Starting/Recovering 中の
	// 一括無効化を行う。
	output_instance_->Restart(overrides);

	// Slice 9(REQ-COMPAT-001): 出力先(device_id/device_name)は独立出力/互換モード共通の設定。
	// 互換モード有効中の変更も即座に反映する(Restart() の反映は control thread 側で非同期に行われる
	// ため GetEffectiveConfig() を使わず、ここで確定した値をそのまま渡す)。
	// レビュー3回目指摘(Medium-2 対応の副作用): デバイス変更は Enable() 内部で「Disable() →
	// Enable(新設定)」の直列実行になったため(旧トラックのソース残留防止)、既に有効中でも
	// Enable() が独立出力の停止待ちタイムアウト等で失敗しうる。他の呼び出し箇所
	// (OnStartStopClicked()/OnOutputModeChanged() 等)と同様に戻り値を見て巻き戻す。
	if (compat_controller_.IsEnabled()) {
		// レビュー3回目残作業(Medium-3b): Enable() 呼び出し直前(内部の Stop() で状態が変わる前)の
		// 独立出力状態を保持する。
		const bool was_running = IsOutputActiveState(output_instance_->GetState());
		if (compat_controller_.Enable(audio_track_spin_->value(), *overrides.device_id,
					      *overrides.device_name)) {
			// レビュー最終指摘(Low-3): Enable() は排他制御のため内部で output_instance_->Stop() を
			// 呼ぶことがあり、Stop() はユーザーの明示的な「止める」意思とみなして enabled=false を
			// 同期的に永続化する(output-instance.cpp Stop() 参照)。互換モードのままデバイス変更が
			// 成功した場合、ユーザーの実際の意図は「互換モードは有効のまま」なので enabled=true を
			// 再度明示的に永続化する(成功時のみ。High-1 の「ユーザー意思のみ」規約と矛盾しない)。
			output_instance_->SetPersistedEnabled(true);
		} else {
			obs_log(LOG_ERROR, "ExternalOutputDock: failed to re-apply compat mode after device change "
					   "(independent output did not stop in time)");
			RollbackCompatModeToIndependent(was_running);
		}
		output_instance_->SetCompatEventCounts(compat_controller_.ApplyCount(),
						       compat_controller_.RestoreCount(),
						       compat_controller_.ReapplyCount());
		UpdateStateDisplay();
		UpdateMonitorWarnings();
	}
}

void ExternalOutputDock::OnRefreshDevicesClicked()
{
	if (exiting_)
		return;
	RefreshDeviceList();
}

void ExternalOutputDock::OnStartStopClicked()
{
	if (exiting_)
		return;

	// Slice 9(REQ-COMPAT-001): 互換モード中はこのボタンが「有効化/無効化」トグルになる(排他、
	// slice-9.md 実装内容の要点)。OutputInstance には一切触れない(独立出力は起動しない)。
	if (IsCompatModeSelected()) {
		// レビュー指摘(High-3): 独立出力が Stopped でない異常(Enable() の停止待ちタイムアウト
		// 直後等)なら、compat の Enable/Disable ではなく独立出力の Stop() を直接呼べるようにする
		// (UpdateCompatStateDisplay() の異常表示と対)。
		if (output_instance_->GetState() != output_instance::State::Stopped) {
			output_instance_->Stop();
			UpdateStateDisplay();
			return;
		}

		if (compat_controller_.IsEnabled()) {
			compat_controller_.Disable();
			// レビュー指摘(High-1): ユーザーによる明示的な無効化。独立出力と同じ「ユーザー意思の
			// み」規約で enabled=false を永続化する。
			output_instance_->SetPersistedEnabled(false);
		} else {
			const output_instance::ConfigValues effective = output_instance_->GetEffectiveConfig();
			// レビュー3回目残作業(Medium-3b): Enable() 呼び出し直前(内部の Stop() で状態が変わる前)
			// の独立出力状態を保持する。
			const bool was_running = IsOutputActiveState(output_instance_->GetState());
			if (compat_controller_.Enable(effective.audio_track, effective.device_id,
						      effective.device_name)) {
				// レビュー指摘(High-1): 有効化成功時に enabled=true を永続化する。
				output_instance_->SetPersistedEnabled(true);
			} else {
				obs_log(LOG_ERROR, "ExternalOutputDock: failed to enable compat mode (independent "
						   "output did not stop in time)");
				// レビュー指摘(High-3): 適用に失敗したら output_mode を independent へ巻き戻す。
				RollbackCompatModeToIndependent(was_running);
			}
		}
		output_instance_->SetCompatEventCounts(compat_controller_.ApplyCount(),
						       compat_controller_.RestoreCount(),
						       compat_controller_.ReapplyCount());
		UpdateStateDisplay();
		UpdateMonitorWarnings();
		return;
	}

	// Slice 6: Start()/Stop() が非同期(enqueue のみ)になり Starting/Recovering が実際に観測可能に
	// なったため、「Starting/Recovering 中の押下 = キャンセル」の分岐を復活させる(旧レビュー指摘
	// L-12 のコメントで予告していたとおり)。Running/Starting/Recovering は Stop(= キャンセル)、
	// Stopped/Failed は Start(Failed からは「再接続」ボタンとして機能。ReconnectButton の文言は
	// UpdateStateDisplay() 側で出し分ける)。
	// レビュー指摘(Medium-6): クリック時点で GetState() を取り直さず、直近の表示に使った state
	// (last_displayed_state_、UpdateStateDisplay() が更新)で分岐する。連打の 2 発目が、表示と
	// クリック処理の間に進んだ状態を読んでボタンラベルと矛盾する動作を取ってしまうのを防ぐ。
	if (last_displayed_state_ == output_instance::State::Running ||
	    last_displayed_state_ == output_instance::State::Starting ||
	    last_displayed_state_ == output_instance::State::Recovering)
		output_instance_->Stop();
	else
		output_instance_->Start();

	// レビュー指摘(Medium-6): 連打の 2 発目誤爆対策。送出直後 1 秒はボタンを disable し、
	// UpdateStateDisplay() の次回ポーリングで再 enable する。
	start_stop_command_pending_ = true;
	start_stop_command_sent_at_ = std::chrono::steady_clock::now();
	start_stop_button_->setEnabled(false);

	// 状態表示は次のポーリング(500ms、OnStatePollTimer())で反映される。enqueue しただけの時点では
	// 実際の遷移がまだ起きていないため、ここで UpdateStateDisplay() を呼んでも意味がない(構造的な
	// 非同期化、S6)。
}

void ExternalOutputDock::OnCopyDiagnosticsClicked()
{
	if (exiting_)
		return;
	const diagnostics::DiagnosticsSnapshot snapshot = output_instance_->BuildDiagnosticsSnapshot();
	const std::string text = diagnostics::BuildDiagnosticsText(snapshot);
	QGuiApplication::clipboard()->setText(QString::fromUtf8(text.c_str()));
}

void ExternalOutputDock::OnAudioTrackChanged(int value)
{
	if (exiting_)
		return;
	// レビュー指摘 H-1: 実効値への反映(Restart())はここでは行わない(OnAudioTrackEditingFinished()
	// 参照)。ここでは値の変化を即座にオンボーディングヒントへ反映するだけ。
	UNUSED_PARAMETER(value);
	UpdateOnboardingHint();
}

void ExternalOutputDock::OnAudioTrackEditingFinished()
{
	if (exiting_)
		return;
	pending_restart_overrides_.audio_track = audio_track_spin_->value();
	restart_debounce_timer_.start(400);
}

void ExternalOutputDock::OnBufferPresetChanged(int index)
{
	if (exiting_)
		return;

	static const char *const kPresetIds[] = {"auto", "low_latency", "stable", "custom"};
	if (index < 0 || index >= 4)
		return;

	const bool is_custom = (index == 3);
	// レイアウト再設計(2026-08-31、実機フィードバック): disable ではなく行ごと非表示にする
	// (UpdateBufferChunksRowVisibility() 参照)。currentData() はこの時点で既に新インデックスの値を
	// 返すため、is_custom をそのまま使わずヘルパー経由で判定を一元化する。
	UpdateBufferChunksRowVisibility();

	output_instance::ConfigOverrides overrides;
	overrides.buffer_preset = std::string(kPresetIds[index]);
	if (is_custom)
		overrides.buffer_chunks = (uint32_t)buffer_chunks_slider_->value();

	// Slice 6: プリセット切替は明示的な単発選択なのでデバウンスせず即時反映する(enqueue のみ、
	// 即座に return)。UX 改定(2026-08-31): ラジオ → QComboBox に置換したが、選択即反映の挙動は不変。
	output_instance_->Restart(overrides);
}

void ExternalOutputDock::OnBufferChunksSliderChanged(int value)
{
	if (exiting_)
		return;

	buffer_chunks_value_label_->setText(Tr("ExternalAudioOutput.Dock.Advanced.BufferChunksValue").arg(value));
	// レビュー指摘 H-1: 実効値への反映(Restart())はドラッグ終了(sliderReleased、
	// OnBufferChunksSliderReleased())のデバウンス経由でのみ行う。ドラッグ中は表示ラベルだけ更新する。
}

void ExternalOutputDock::OnBufferChunksSliderReleased()
{
	if (exiting_)
		return;
	if (buffer_preset_combo_->currentIndex() != 3)
		return; // "custom" 未選択中はスライダー値を実効値へ反映しない(既存方針を踏襲)。

	pending_restart_overrides_.buffer_chunks = (uint32_t)buffer_chunks_slider_->value();
	restart_debounce_timer_.start(400);
}

void ExternalOutputDock::OnRestartDebounceTimeout()
{
	if (exiting_)
		return;

	const output_instance::ConfigOverrides overrides = pending_restart_overrides_;
	pending_restart_overrides_ = output_instance::ConfigOverrides();

	output_instance_->Restart(overrides);

	// Slice 9(REQ-COMPAT-001): audio_track は独立出力/互換モード共通の設定。互換モード有効中に
	// 変更された場合も即座に反映する(出力先は現在の compat_controller_ の値をそのまま使う。
	// buffer_chunks 等の変更はここを通っても互換モードには無関係なので無視してよい)。
	// レビュー3回目指摘(Medium-2 対応の副作用): トラック変更は Enable() 内部で「Disable() →
	// Enable(新設定)」の直列実行になったため、既に有効中でも失敗しうる。他の呼び出し箇所と同様に
	// 戻り値を見て巻き戻す。
	if (overrides.audio_track.has_value() && compat_controller_.IsEnabled()) {
		// レビュー3回目残作業(Medium-3b): Enable() 呼び出し直前(内部の Stop() で状態が変わる前)の
		// 独立出力状態を保持する。
		const bool was_running = IsOutputActiveState(output_instance_->GetState());
		if (compat_controller_.Enable(*overrides.audio_track, compat_controller_.OutputDeviceId(),
					      compat_controller_.OutputDeviceName())) {
			// レビュー最終指摘(Low-3): Enable() 内部の Stop() が enabled=false を残しうるため、
			// 互換モードのままトラック変更が成功した場合は enabled=true を再度明示的に永続化する
			// (OnDeviceComboActivated() と同じ理由)。
			output_instance_->SetPersistedEnabled(true);
		} else {
			obs_log(LOG_ERROR, "ExternalOutputDock: failed to re-apply compat mode after audio track "
					   "change (independent output did not stop in time)");
			RollbackCompatModeToIndependent(was_running);
		}
		output_instance_->SetCompatEventCounts(compat_controller_.ApplyCount(),
						       compat_controller_.RestoreCount(),
						       compat_controller_.ReapplyCount());
		UpdateStateDisplay();
		UpdateMonitorWarnings();
	}
}

void ExternalOutputDock::OnAutoReconnectToggled(bool checked)
{
	if (exiting_)
		return;

	// Slice 6: auto_reconnect は実際に動作へ反映される(切断検知時に Recovering/Failed のどちらへ
	// 遷移するか)。値の変更自体は出力の再起動を要さないため Restart() は audio_track 等と異なり
	// no-op 判定(HandleRestartCommand())で再起動されない(値のみ更新)。
	output_instance::ConfigOverrides overrides;
	overrides.auto_reconnect = checked;
	output_instance_->Restart(overrides);
}

void ExternalOutputDock::OnAdvancedToggled(bool checked)
{
	if (exiting_)
		return;
	advanced_panel_->setVisible(checked);
	advanced_toggle_->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
}

void ExternalOutputDock::OnOutputModeChanged(int index)
{
	if (exiting_)
		return;

	// UX 改定(2026-08-31): 説明ラベルは実際の適用可否に関わらず、選択中アイテムに追従して更新する。
	UpdateOutputModeDescription();

	const std::string new_mode = index == 1 ? "obs_monitor_compat" : "independent";
	const std::string current_mode = plugin_config::Load().output_mode;
	if (new_mode == current_mode)
		return;

	// REQ-COMPAT-001 制約5(排他): 「現モード完全停止→新モード適用」を直列に行う。
	// レビュー最終指摘(Low-1): ディスク上の current_mode ではなく、実際にランタイムで互換モードが
	// 有効かどうか(compat_controller_.IsEnabled())で判定する。current_mode はコンボ操作の合間に
	// ApplyStartupCompatState()/OnSceneCollectionChanging()/RollbackCompatModeToIndependent() 等で
	// 書き換わりうるため、ディスク値と in-memory の compat_controller_ の実状態がずれるケース
	// (compat_controller_ は有効なままなのに current_mode は既に "independent" になっている等)を
	// current_mode だけに頼ると見逃す。
	if (compat_controller_.IsEnabled()) {
		compat_controller_.Disable();
		// レビュー指摘(High-1): ユーザーによる明示的な無効化(モード切替も含む)。独立出力と同じ
		// 「ユーザー意思のみ」規約で enabled=false を永続化する。
		output_instance_->SetPersistedEnabled(false);
	}
	// independent 側の停止は MonitorCompatController::Enable() が内部で行う(排他ガード)ため、
	// ここでは output_instance_->Stop() を呼ばない。

	output_instance_->SetPersistedOutputMode(new_mode);

	if (new_mode == "obs_monitor_compat") {
		const output_instance::ConfigValues effective = output_instance_->GetEffectiveConfig();
		// レビュー3回目残作業(Medium-3b): Enable() 呼び出し直前(内部の Stop() で状態が変わる前)の
		// 独立出力状態を保持する。このパスは「独立出力から互換モードへ切り替える」まさにその操作
		// なので、was_running=true になりうる本命のケース。
		const bool was_running = IsOutputActiveState(output_instance_->GetState());
		if (compat_controller_.Enable(effective.audio_track, effective.device_id, effective.device_name)) {
			// レビュー指摘(High-1): 有効化成功時に enabled=true を永続化する。
			output_instance_->SetPersistedEnabled(true);
		} else {
			obs_log(LOG_ERROR, "ExternalOutputDock: failed to enable compat mode (independent output "
					   "did not stop in time)");
			// レビュー指摘(High-3): 適用に失敗したら output_mode を independent へ巻き戻す
			// (「見えず止められない出力」を防ぐ)。
			RollbackCompatModeToIndependent(was_running);
		}
	}
	output_instance_->SetCompatEventCounts(compat_controller_.ApplyCount(), compat_controller_.RestoreCount(),
					       compat_controller_.ReapplyCount());

	UpdateStateDisplay();
	UpdateMonitorWarnings();
}

void ExternalOutputDock::RollbackCompatModeToIndependent(bool was_output_running_before_enable)
{
	// レビュー指摘(High-3): MonitorCompatController::Enable() が失敗した(独立出力の停止待ち
	// タイムアウト等)場合、永続 output_mode と OutputMode コンボを "independent" へ巻き戻す。
	// 巻き戻さないと、永続 output_mode=compat のまま独立出力が動き続ける「見えず止められない出力」
	// が残る(UpdateCompatStateDisplay() の異常検知はこれを検出して Stop() ボタンを出すための保険)。
	obs_log(LOG_WARNING,
		"ExternalOutputDock: rolling back output_mode to \"independent\" after a failed compat mode enable "
		"(was_running_before=%s)",
		was_output_running_before_enable ? "true" : "false");
	output_instance_->SetPersistedOutputMode("independent");
	output_mode_combo_->blockSignals(true);
	output_mode_combo_->setCurrentIndex(0);
	output_mode_combo_->blockSignals(false);
	// blockSignals 中は OnOutputModeChanged() が呼ばれず説明ラベルが追従しないため明示的に更新する
	// (UX 改定 2026-08-31)。
	UpdateOutputModeDescription();

	// レビュー3回目残作業(Medium-3b): 切替前に独立出力が動作中(Running/Starting/Recovering、
	// レビュー最終指摘 Low-5)だった場合のみ、独立出力の Start を再 enqueue して元の実行状態を復元
	// する。MonitorCompatController::Enable() は排他制御のため呼び出し冒頭で output_instance_->Stop()
	// を enqueue 済み(UI 側の停止待ちがタイムアウトしただけで、control queue 上の Stop コマンド自体は
	// 取り消されていない)。control thread は queue を FIFO で直列処理するため、ここで追加 enqueue する
	// Start は先行する Stop の完了を待ってから実行される(HandleRestartCommand と同じ安全性の根拠)。
	// OutputInstance::Stop() はユーザーの明示的な「止める」意思とみなして enabled=false を同期的に
	// 永続化する(output-instance.cpp Stop() 参照、Slice 7)ため、Enable() 内の Stop() 呼び出しに
	// よって enabled=false が既に書き込まれている。巻き戻しではユーザーが本来意図していた「出力中」
	// の状態を復元するため、ここで明示的に enabled=true へ書き戻す。
	if (was_output_running_before_enable) {
		output_instance_->Start();
		output_instance_->SetPersistedEnabled(true);
	} else {
		// レビュー最終指摘(Medium-B): 切替前に独立出力が動作していなかった場合、上の Start()
		// パスを通らないため enabled は書き換わらない。ここで enabled=true のまま残っていると
		// (例: ApplyStartupCompatState() の起動時自動適用が失敗したケース。config.enabled=true
		// のまま output_mode だけ independent へ巻き戻る)、次回起動時に意図しない独立出力の
		// 自動開始(plugin-main.cpp の FINISHED_LOADING)を招く。明示的に enabled=false を
		// 永続化して防ぐ。
		output_instance_->SetPersistedEnabled(false);
	}

	// レビュー3回目残作業(Medium-3a): Dock に「互換モードへの切替に失敗したため独立出力に戻した」旨を
	// 1 回だけ表示する(次回 UpdateMonitorWarnings() で monitor_only_warning_label_ を流用。次回の
	// 状態変化で消えてよい)。
	pending_compat_rollback_warning_ = Tr("ExternalAudioOutput.Dock.Compat.RollbackToIndependentWarning");

	// レビュー最終指摘(Low-2): UpdateMonitorWarnings() の次回呼び出し(最大 2 秒後の
	// monitor_only_timer_ tick)を待たずに、この場で即座に表示する。ApplyStartupCompatState()
	// (起動時の自動適用失敗)はこの後 UpdateMonitorWarnings() を呼ばないため、そのままだと起動直後に
	// ユーザーへ警告が見えない。消費ロジック(pending_compat_rollback_warning_ を次回
	// UpdateMonitorWarnings() で 1 回だけ表示してクリアする)自体は変更しない。
	monitor_only_warning_label_->setText(pending_compat_rollback_warning_);
	monitor_only_warning_label_->setVisible(true);
}

void ExternalOutputDock::ApplyStartupCompatState()
{
	// REQ-COMPAT-001: plugin-main.cpp の OBS_FRONTEND_EVENT_FINISHED_LOADING から呼ぶ(1 回のみ)。
	// レビュー指摘(追加ガード、S9-11): 起動時に SCENE_COLLECTION_CHANGING/CHANGED が
	// FINISHED_LOADING より前に 2 回発火するため、それらのハンドラはこのフラグが立つまで無視する
	// (ソース未ロード段階で Enable()/Disable() が走り不完全な restore file が作られるのを防ぐ)。
	finished_loading_received_ = true;

	const plugin_config::PluginConfig config = plugin_config::Load();
	if (config.output_mode == "obs_monitor_compat") {
		if (!config.enabled) {
			obs_log(LOG_DEBUG, "ExternalOutputDock: output_mode=\"obs_monitor_compat\" but enabled=false; "
					   "skipping auto-apply at startup");
			// レビュー指摘(High-1): enabled=false でも compat-restore.json が残っていれば起動時
			// 復元する(Critical-1 の「欠落/失敗時はファイルを残す」変更と組み合わさると、
			// 無効化状態のまま残骸が回収されず残り続けるケースがあるため)。
			compat_controller_.RestoreStaleFileIfPresent();
			output_instance_->SetCompatEventCounts(compat_controller_.ApplyCount(),
							       compat_controller_.RestoreCount(),
							       compat_controller_.ReapplyCount());
			return;
		}
		const output_instance::ConfigValues effective = output_instance_->GetEffectiveConfig();
		obs_log(LOG_INFO,
			"ExternalOutputDock: auto-applying compat mode at startup (track=%d device_id=\"%s\")",
			effective.audio_track, effective.device_id.c_str());
		// レビュー3回目残作業(Medium-3b): output_mode=compat の起動時は独立出力の自動開始自体が
		// スキップされている(plugin-main.cpp FINISHED_LOADING)ため、通常は false のはずだが、
		// 呼び出し直前の実状態を見て他の呼び出し箇所と同じ規約に揃える。
		const bool was_running = IsOutputActiveState(output_instance_->GetState());
		if (!compat_controller_.Enable(effective.audio_track, effective.device_id, effective.device_name)) {
			obs_log(LOG_ERROR, "ExternalOutputDock: failed to auto-apply compat mode at startup "
					   "(independent output did not stop in time)");
			// レビュー指摘(High-3): 起動時の自動適用が失敗した場合も同じ巻き戻しを行う。
			RollbackCompatModeToIndependent(was_running);
		}
	} else {
		// クラッシュ耐性(S9-7): output_mode!=compat なのに compat-restore.json が残っていれば
		// ファイルから直接復元する(前回セッションが EXIT を経ずに終了したケース)。
		compat_controller_.RestoreStaleFileIfPresent();
	}
	output_instance_->SetCompatEventCounts(compat_controller_.ApplyCount(), compat_controller_.RestoreCount(),
					       compat_controller_.ReapplyCount());
}

void ExternalOutputDock::OnSceneCollectionChanging()
{
	if (exiting_)
		return;
	// レビュー指摘(追加ガード、S9-11): FINISHED_LOADING 受領前(起動時の 2 回の早期発火)は無視する。
	if (!finished_loading_received_)
		return;
	// レビュー指摘(Critical-1b): シーンコレクションが切り替わる前に、現在のコレクションのソースを
	// 対象として復元しておく(切替後は対象ソースが未ロードになり、compat-restore.json が残っていても
	// obs_get_source_by_name() で見つからず復元漏れになる、S9-7)。
	if (compat_controller_.IsEnabled())
		compat_controller_.Disable();
	output_instance_->SetCompatEventCounts(compat_controller_.ApplyCount(), compat_controller_.RestoreCount(),
					       compat_controller_.ReapplyCount());
	UpdateStateDisplay();
	UpdateMonitorWarnings();
}

void ExternalOutputDock::OnSceneCollectionChanged()
{
	if (exiting_)
		return;
	// レビュー指摘(追加ガード、S9-11): FINISHED_LOADING 受領前(起動時の 2 回の早期発火)は無視する。
	if (!finished_loading_received_)
		return;
	// レビュー指摘(Critical-1b): 新しいシーンコレクションの読み込み後、output_mode=compat かつ
	// enabled のままなら新コレクションのソースへ再適用する(OnSceneCollectionChanging() で
	// Disable() 済みのため、通常の Enable() 経路をそのまま使える)。
	const plugin_config::PluginConfig config = plugin_config::Load();
	if (config.output_mode == "obs_monitor_compat" && config.enabled) {
		const output_instance::ConfigValues effective = output_instance_->GetEffectiveConfig();
		// レビュー3回目残作業(Medium-3b): Enable() 呼び出し直前(内部の Stop() で状態が変わる前)の
		// 独立出力状態を保持する。
		const bool was_running = IsOutputActiveState(output_instance_->GetState());
		if (!compat_controller_.Enable(effective.audio_track, effective.device_id, effective.device_name)) {
			obs_log(LOG_ERROR, "ExternalOutputDock: failed to re-apply compat mode after scene "
					   "collection change (independent output did not stop in time)");
			RollbackCompatModeToIndependent(was_running);
		}
	} else {
		// output_mode!=compat、または enabled=false: 万一 compat-restore.json が残っていれば
		// クラッシュ耐性と同じ経路で回収する。
		compat_controller_.RestoreStaleFileIfPresent();
	}
	output_instance_->SetCompatEventCounts(compat_controller_.ApplyCount(), compat_controller_.RestoreCount(),
					       compat_controller_.ReapplyCount());
	UpdateStateDisplay();
	UpdateMonitorWarnings();
}

void ExternalOutputDock::OnProfileChangedCompatReapply()
{
	if (exiting_)
		return;
	if (!compat_controller_.IsEnabled())
		return;
	compat_controller_.Reapply();
	output_instance_->SetCompatEventCounts(compat_controller_.ApplyCount(), compat_controller_.RestoreCount(),
					       compat_controller_.ReapplyCount());
}
