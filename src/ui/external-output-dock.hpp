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
// / Slice 5, Slice 6
//
// External Audio Output の Dock 本体(Qt Widgets、REQUIREMENTS.md §5 の構成)。OBS が所有する
// QWidget(qt-ui-and-lifecycle.md: delete しない、obs_frontend_add_dock_by_id に渡した後は
// プラグイン側で破棄しない)。
//
// スレッド規約(rt-audio.md/qt-ui-and-lifecycle.md、slice-5.md 実装内容 9/10、Slice 6 で更新):
//   - render thread(WASAPI)→ UI: std::atomic のみ(WasapiOutput::LevelPeakL/R() 経由、
//     OutputInstance::ui_mutex_ で短時間保護)。
//   - UI → OutputInstance: Start()/Stop()/Restart()/RequestReregister() はいずれもコマンドを
//     ControlQueue へ積んで即座に return する(Slice 6: OutputInstance の control thread が非同期に
//     処理する。呼び出し元をブロックしない)。結果は GetState() の 500ms ポーリングで観測する
//     (UpdateStateDisplay() が Starting/Recovering 中はデバイスコンボ・詳細設定パネルを無効化する)。
//   - Dock から WASAPI/COM を直接呼ばない。デバイス列挙は AudioDeviceManager::Enumerate() 経由
//     (内部で COM が完結する。Qt UI スレッド(STA)からの呼び出しに対応済み、wasapi-com.md)。
//   - すべてのウィジェット操作・タイマーコールバックは Qt UI スレッド上で完結する。
//   - EXIT(OBS_FRONTEND_EVENT_EXIT)時は OnExit() で QTimer を止めるだけ(Dock 自体は delete しない)。

#pragma once

#include "../compat/monitor-compat-controller.hpp"
#include "../output/output-instance.hpp"

#include <QComboBox>
#include <QTimer>
#include <QWidget>

#include <chrono>
#include <cstdint>

class QLabel;
class QPushButton;
class QToolButton;
class QSpinBox;
class QSlider;
class QCheckBox;
class QFormLayout;

// L/R の Level Meter を描画する小さな QWidget(D-15、REQUIREMENTS.md §5: -60〜0dBFS)。
// SetLevels() は Qt UI スレッド(ExternalOutputDock の QTimer)からのみ呼ぶ。paintEvent 以外は
// シグナル/スロットを持たないため Q_OBJECT は不要。
class LevelMeterWidget : public QWidget {
public:
	explicit LevelMeterWidget(QWidget *parent = nullptr);

	// display_*_db: 現在表示すべきバー先端の dBFS(-60〜0 にクランプ済みであること)。
	// peak_hold_*_db: ピークホールド表示用の細い目印線の dBFS。
	void SetLevels(float display_l_db, float display_r_db, float peak_hold_l_db, float peak_hold_r_db);

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	float display_l_db_ = -60.0f;
	float display_r_db_ = -60.0f;
	float peak_hold_l_db_ = -60.0f;
	float peak_hold_r_db_ = -60.0f;
};

// 出力先ドロップダウン。ポップアップを開くたびに再列挙する(REQUIREMENTS.md §5: 「開いたとき/
// 更新ボタンで再列挙」)。シグナルを追加するため QComboBox のサブクラスにする(Q_OBJECT はネストした
// クラスでは使えないため、ExternalOutputDock とは別のトップレベルクラスにする)。
class DeviceComboBox : public QComboBox {
	Q_OBJECT
public:
	explicit DeviceComboBox(QWidget *parent = nullptr);

signals:
	void AboutToOpenPopup();

protected:
	void showPopup() override;
};

class ExternalOutputDock : public QWidget {
	Q_OBJECT
public:
	explicit ExternalOutputDock(OutputInstance *output_instance, QWidget *parent = nullptr);
	~ExternalOutputDock() override;

	ExternalOutputDock(const ExternalOutputDock &) = delete;
	ExternalOutputDock &operator=(const ExternalOutputDock &) = delete;

	// OBS_FRONTEND_EVENT_EXIT で plugin-main.cpp から呼ぶ。QTimer を止めるだけ
	// (qt-ui-and-lifecycle.md: EXIT 時は QTimer 停止。OutputInstance::Stop() は別途 EXIT ハンドラが
	// 呼ぶ)。Dock ウィジェット自体は delete しない(OBS 所有)。二重に呼んでも安全(QTimer::stop()
	// は冪等)。
	void OnExit();

	// Slice 9(REQ-COMPAT-001): plugin-main.cpp の OBS_FRONTEND_EVENT_FINISHED_LOADING から呼ぶ。
	// output_mode="obs_monitor_compat" かつ enabled=true なら自動適用し、それ以外(output_mode=
	// "independent")のときは compat-restore.json の残骸(クラッシュ耐性、S9-7)を起動時復元する。
	void ApplyStartupCompatState();

	// plugin-main.cpp の OBS_FRONTEND_EVENT_PROFILE_CHANGED から呼ぶ(REQ-COMPAT-001 制約3、S9-3)。
	// 互換モード有効中でなければ no-op。
	void OnProfileChangedCompatReapply();

	// レビュー指摘(Critical-1b): plugin-main.cpp の
	// OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING/CHANGED から呼ぶ。monitoring_type はシーン
	// コレクションに永続化される(S9-7)ため、切替でソースが未ロードになる前に復元しておかないと
	// 復元情報が対象を失う。CHANGING で復元(Disable() 相当)、CHANGED で(compat 設定がまだ有効な
	// ままなら)新コレクションのソースへ再適用する。plugin-main.cpp は frontend イベントスレッド上で
	// 直列に呼ぶ。
	void OnSceneCollectionChanging();
	void OnSceneCollectionChanged();

private slots:
	void OnStatePollTimer();
	void OnMeterPollTimer();
	void OnMonitorOnlyPollTimer();

	void OnDeviceComboAboutToOpen();
	void OnDeviceComboActivated(int index);
	void OnRefreshDevicesClicked();
	void OnStartStopClicked();
	void OnCopyDiagnosticsClicked();

	void OnAudioTrackChanged(int value);
	// UX 改定(2026-08-31): ラジオ 4 択 → QComboBox に置換(他項目と統一、REQUIREMENTS.md §5)。
	void OnBufferPresetChanged(int index);
	void OnBufferChunksSliderChanged(int value);
	void OnAutoReconnectToggled(bool checked);
	void OnAdvancedToggled(bool checked);
	// Slice 9(REQ-COMPAT-001): OutputMode コンボ(独立出力/互換モード)の切替。制約5(排他)に従い
	// 「現モード完全停止→新モード適用」を直列に行う。
	void OnOutputModeChanged(int index);

	// レビュー指摘 H-1: スライダー/スピンの連続した値変更のたびに Restart() しないよう、確定操作
	// (ドラッグ終了/フォーカスアウト)でだけ pending_restart_overrides_ に差分を積み、単発デバウンス
	// タイマーへ回す。
	void OnAudioTrackEditingFinished();
	void OnBufferChunksSliderReleased();
	void OnRestartDebounceTimeout();

	// レビュー指摘 H-2: コンストラクタでの初回デバイス列挙(COM 呼び出しを伴う)を次のイベント
	// ループへ遅延する(QTimer::singleShot(0, ...))。
	void OnInitialDeviceEnumeration();

private:
	void BuildUi();
	// AudioDeviceManager::Enumerate() を呼んでコンボボックスを再構築する。prefer_device_id が
	// 空でなければその device_id を選択対象にする(初回列挙・config.json 復元用)。空なら現在
	// コンボが選択している device_id を可能な限り保持する(ポップアップ再列挙/更新ボタン用)。
	// prefer_device_name は prefer_device_id が今回の列挙に無かった場合のプレースホルダー表示名
	// (レビュー指摘 M-5)。UI スレッドから直接呼ぶ(COM は AudioDeviceManager 内部で完結)。
	void RefreshDeviceList(const QString &prefer_device_id = QString(),
			       const QString &prefer_device_name = QString());
	void UpdateStateDisplay();
	void UpdateOnboardingHint();
	// OutputInstance::GetEffectiveConfig() を読み、詳細設定パネル(音声トラック/バッファ/自動再接続)の
	// 初期表示に反映する(デバイスコンボの選択は含まない。そちらは RefreshDeviceList() の責務。
	// コンストラクタから 1 回だけ呼ぶ。以後は Dock 自身の操作が真値になる)。ログレベルは UX 改定
	// (2026-08-31)で UI から削除済み(config.json の手編集でのみ変更可能、REQUIREMENTS.md §5)。
	void ApplyEffectiveConfigToUi();
	// output_mode_combo_ の選択中インデックスに応じて、コンボ直下の説明ラベル(1 行)を切り替える
	// (UX 改定 2026-08-31、REQUIREMENTS.md §5「出力方式の説明」)。
	void UpdateOutputModeDescription();
	// レイアウト再設計(2026-08-31、実機フィードバック): buffer_preset_combo_ が "custom" のときだけ
	// チャンク行(スライダー+値ラベル)を表示する。setRowVisible で行ごと隠すため、非表示時は縦方向にも
	// 詰まる(disable するだけの旧実装と異なる)。ApplyEffectiveConfigToUi()/OnBufferPresetChanged() から
	// 呼ぶ。
	void UpdateBufferChunksRowVisibility();
	// メーター 1 チャンネル分の UI 側減衰(20dB/s)・ピークホールド(1s)を計算する(D-15)。
	void UpdateMeterChannel(float new_db, float &display_db, float &peak_hold_db,
				int &peak_hold_remaining_ms) const;
	// meter_timer_ を止めてメーター表示をフロアへ戻す(Running を離れたとき/互換モード中の共通処理)。
	void ResetMeterToFloor();
	// 互換モードが有効なときの状態表示・警告更新(UpdateStateDisplay() から分岐して呼ぶ)。
	void UpdateCompatStateDisplay();
	// 制約1/4/5 の警告ラベル(MONITOR_ONLY 未変更・自己無音)を更新する(OnMonitorOnlyPollTimer() から
	// 独立出力/互換モードどちらでも呼ぶ)。
	void UpdateMonitorWarnings();
	bool IsCompatModeSelected() const;
	// レビュー指摘(High-3): MonitorCompatController::Enable() が失敗した(独立出力の停止待ち
	// タイムアウト等)場合に、永続 output_mode と OutputMode コンボを "independent" へ巻き戻す共通
	// 処理。巻き戻さないと、永続 output_mode=compat のまま独立出力が動き続ける「見えず止められない
	// 出力」が残る。
	// レビュー3回目残作業(Medium-3): was_output_running_before_enable は呼び出し元が
	// compat_controller_.Enable() を呼ぶ「直前」に output_instance_->GetState() を見て渡す
	// (Enable() 内部の Stop() で状態が変わってしまう前の値)。true のときは切替前に独立出力が
	// Running だったとみなし、独立出力の Start() を再 enqueue して enabled=true を永続化する
	// (control queue は FIFO のため、Enable() が先に enqueue した Stop の完了後に実行される。
	// HandleRestartCommand と同じ直列化パターン)。
	void RollbackCompatModeToIndependent(bool was_output_running_before_enable);

	OutputInstance *output_instance_ = nullptr; // 非所有(生存はプラグイン全体と同じ)
	// Slice 9(REQ-COMPAT-001): 互換モードの実体。UI スレッドでのみ操作する(有効化/無効化/再適用は
	// すべてこの Dock のスロット/タイマー/plugin-main.cpp 経由の frontend イベントから呼ばれる)。
	MonitorCompatController compat_controller_;

	DeviceComboBox *device_combo_ = nullptr;
	QToolButton *refresh_devices_button_ = nullptr;
	QLabel *state_label_ = nullptr;
	LevelMeterWidget *level_meter_ = nullptr;
	QPushButton *start_stop_button_ = nullptr;
	QLabel *monitor_only_warning_label_ = nullptr;
	// レビュー3回目残作業(Medium-3a): RollbackCompatModeToIndependent() が設定する一回限りの警告
	// メッセージ。次回 UpdateMonitorWarnings() 呼び出し(state_label_ 等と異なり通常の 2 秒ポーリング/
	// 各種イベントハンドラの直後にしか呼ばれないため、この Dock 内でユーザーへ見せる猶予がある)で
	// monitor_only_warning_label_ に 1 回だけ表示し、表示後は空へ戻す(=「次回状態変化で消えてよい」)。
	// 空文字列は「保留中の警告なし」。
	QString pending_compat_rollback_warning_;
	// Slice 9(03-compat-mode-reassessment.md 必須制約5、事実5 DO_NOT_SELF_MONITOR): 自己無音警告。
	// REQ-COMPAT-001 自体の 5 項制約(正本)には含まれない別出典(REQ-COMPAT-001 制約5 は排他)。既存の
	// monitor_only_warning_label_ とは別ラベル(文言・意味が異なるため)。
	QLabel *compat_self_silence_warning_label_ = nullptr;
	// Slice 9(REQ-COMPAT-001 制約4): 互換モード有効中の常時警告(既定非表示、locale キーは
	// 既存の Dock.CompatModeWarning)。
	QLabel *compat_mode_warning_label_ = nullptr;
	// レビュー指摘(Low-3): 互換モード有効中、既にモニタリング設定済みの他ソースの音も鳴る旨の注記
	// (常時警告ラベルの下、既定非表示)。
	QLabel *compat_other_sources_note_label_ = nullptr;
	QLabel *onboarding_hint_label_ = nullptr;
	// レイアウト再設計(2026-08-31、実機フィードバック): 大きな QPushButton → アイコンのみの小型
	// QToolButton(約 22x22px、autoRaise)に変更。機能・locale キー(CopyDiagnostics)は不変(AC-25)。
	QToolButton *copy_diagnostics_button_ = nullptr;

	QToolButton *advanced_toggle_ = nullptr;
	QWidget *advanced_panel_ = nullptr;
	// レイアウト再設計(2026-08-31): advanced_panel_ に設定した QFormLayout(2 カラム)。
	// UpdateBufferChunksRowVisibility() が setRowVisible() を呼ぶために保持する。
	QFormLayout *advanced_form_layout_ = nullptr;
	QSpinBox *audio_track_spin_ = nullptr;
	// UX 改定(2026-08-31): ラジオ 4 択 → QComboBox に置換(他項目と統一、REQUIREMENTS.md §5)。
	QComboBox *buffer_preset_combo_ = nullptr;
	QSlider *buffer_chunks_slider_ = nullptr;
	QLabel *buffer_chunks_value_label_ = nullptr;
	// レイアウト再設計(2026-08-31): スライダー+値ラベルを 1 行にまとめたコンテナ。
	// buffer_preset_combo_ が "custom" 以外のときは setRowVisible() でこの行ごと隠す(縦方向に詰まる)。
	QWidget *buffer_chunks_row_widget_ = nullptr;
	QCheckBox *auto_reconnect_check_ = nullptr;
	// Slice 9(REQ-COMPAT-001): 出力方式(独立出力/OBS モニタリング互換、REQUIREMENTS.md §5)。
	QComboBox *output_mode_combo_ = nullptr;
	// UX 改定(2026-08-31、実機フィードバック追加分/第2弾): 選択中の出力方式の説明を表示するラベル
	// (小さめ・淡色のフォント)。当初は output_mode_combo_ と同じ QWidget コンテナ(QVBoxLayout)に
	// 入れて QFormLayout の 1 行のフィールド列にまとめていたが、QFormLayout はコンテナ入り行の
	// heightForWidth を正しく計算できず行高が潰れ、コンボ自体が縦に潰れて見切れる不具合が実機で
	// 発生した。素の addRow(ラベル, output_mode_combo_) に戻し、説明ラベルは直後の独立フォーム行
	// (addRow(空ラベル, output_mode_desc_label_))として追加する(フィールド列に揃うため見た目は
	// コンボ直下・同じ左端に並び、帰属は視覚的に分かる)。BuildUi() 参照。
	QLabel *output_mode_desc_label_ = nullptr;

	QTimer state_timer_;
	// レビュー指摘 Medium-1(Slice 8): Running 中のみ動かす(UpdateStateDisplay() が開始/停止する)。
	// アイドル時のプラグイン CPU 消費を測定する際、この 30Hz タイマーが動きっぱなしだと支配的な
	// ノイズ源になってしまうため。
	QTimer meter_timer_;
	QTimer monitor_only_timer_;
	// レビュー指摘 H-1: スライダー/スピンの確定操作(sliderReleased/editingFinished)から実際に
	// Restart() するまでの単発デバウンスタイマー(400ms)。
	QTimer restart_debounce_timer_;

	// メーターの UI 側減衰(20dB/s)・ピークホールド(1s)の状態(D-15、slice-5.md 実装内容 3)。
	float meter_display_l_db_ = -60.0f;
	float meter_display_r_db_ = -60.0f;
	float meter_peak_hold_l_db_ = -60.0f;
	float meter_peak_hold_r_db_ = -60.0f;
	int meter_peak_hold_l_remaining_ms_ = 0;
	int meter_peak_hold_r_remaining_ms_ = 0;

	// ComboBox 再構築中(RefreshDeviceList())は activated シグナルを無視する再入防止フラグ。
	bool device_combo_populating_ = false;

	// レビュー指摘(Medium-6): 開始/停止ボタンの連打対策。
	//   - last_displayed_state_: 直近の UpdateStateDisplay() が実際に表示に使った state。
	//     OnStartStopClicked() はクリック時点で GetState() を取り直さずこの値で分岐する(表示と
	//     クリック処理の間に状態が進んでいても、ユーザーが見たボタンラベルと矛盾しない動作を保証する)。
	//   - start_stop_command_pending_/start_stop_command_sent_at_: コマンド送出直後 1 秒間はボタンを
	//     disable し、UpdateStateDisplay()(500ms ポーリング)側で経過を見て再 enable する。
	output_instance::State last_displayed_state_ = output_instance::State::Stopped;
	bool start_stop_command_pending_ = false;
	std::chrono::steady_clock::time_point start_stop_command_sent_at_{};

	// レビュー指摘 H-1: restart_debounce_timer_ が発火するまでに溜まった差分(複数の確定操作を
	// 1 回の Restart() にまとめる)。OnRestartDebounceTimeout() で消費してクリアする。
	output_instance::ConfigOverrides pending_restart_overrides_;

	// レビュー指摘 M-8: OnExit() 以降、すべての UI イベントハンドラを no-op にする遮断フラグ
	// (setEnabled(false) と二重に保険をかける。EXIT 後にキュー済みのシグナルが配送されても
	// OutputInstance/COM へ触れないようにするため)。
	bool exiting_ = false;

	// レビュー指摘(追加ガード、S9-11): ApplyStartupCompatState()(OBS_FRONTEND_EVENT_FINISHED_LOADING
	// から 1 回だけ呼ばれる)で true になる。起動時に SCENE_COLLECTION_CHANGED が FINISHED_LOADING より
	// 前に 2 回発火するため、OnSceneCollectionChanging()/OnSceneCollectionChanged() はこのフラグが
	// 立つまで no-op にする(ソース未ロード段階で Enable() が走り不完全な restore file が作られるのを
	// 防ぐ)。
	bool finished_loading_received_ = false;
};
