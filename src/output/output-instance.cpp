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

// 対応 REQ: REQ-AUDIO-002, REQ-AUDIO-003, REQ-AUDIO-004, REQ-AUDIO-005, REQ-AUDIO-007,
// REQ-DEVICE-001, REQ-DEVICE-002, REQ-DEVICE-003, REQ-DEVICE-004, REQ-LIFECYCLE-001, REQ-LOG-001,
// REQ-NFR-002, REQ-STABILITY-001, REQ-UI-001, REQ-UI-005, REQ-UI-006, REQ-UI-007, REQ-UI-008 /
// Slice 2, Slice 3, Slice 4, Slice 5, Slice 6

#include "output-instance.hpp"

#include "../audio/track-map.hpp"
#include "../config/plugin-config.hpp"
#include "../device/audio-device-manager.hpp"
#include "../device/device-matcher.hpp"

#include <obs-module.h>
#include <obs.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <mmdeviceapi.h>
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <optional>

namespace {

// AUDIO_OUTPUT_FRAMES(libobs の 1 コールバックあたりの frames)。conversion 後はデバイスレートに
// リサンプルされるため、実際にリングバッファへ積まれる frames はこれをデバイスレート換算した値になる。
constexpr uint32_t kObsFramesPerCallback = 1024;

// レート比の異常値しきい値(レビュー指摘 #2)。これを超える場合は極端な高レート変換とみなし Failed。
constexpr double kMaxRateRatio = 8.0;

// RingBuffer::TryPush の oversize 切り詰めに対するマージン(レビュー指摘 #2)。
constexpr uint32_t kMaxChunkFramesMargin = 64;

// control thread のメインループの WaitPop タイムアウト(ブリーフ: デバウンス 200ms と揃える。
// ウォッチドッグ/reconnect backoff のティックもこの周期で進む)。
constexpr std::chrono::milliseconds kControlLoopPollTimeout(200);

// OnDefaultDeviceChanged のデバウンス(wasapi-com.md/ブリーフ実装内容 7)。
constexpr uint64_t kDefaultDeviceChangeDebounceNs = 200ULL * 1000000ULL;

// ウォッチドッグ(ブリーフ実装内容 11): 500ms 周期でチェックし、1 秒以上無進行なら再登録する。
constexpr uint64_t kWatchdogCheckIntervalNs = 500ULL * 1000000ULL;
constexpr uint64_t kWatchdogStallThresholdNs = 1000ULL * 1000000ULL;

// Slice 8(AC-12): control thread の自己 CPU% サンプリング間隔(diag の 2 秒周期ログと揃える)。
constexpr uint64_t kCpuSampleIntervalNs = 2000ULL * 1000000ULL;

// ピンポイント修正(AC-12): アイドル(Stopped)時の "idle stats" ログの出力間隔。ログスパム防止のため
// 60 秒間隔厳守(diag の Running 中 2 秒周期とは別枠)。
constexpr uint64_t kIdleStatsLogIntervalNs = 60000ULL * 1000000ULL;

// ゾンビ累積上限(3 個)に到達したときのログメッセージ。3 個目発生時と、以後の手動 Start()(「再接続」
// ボタン)要求の両方でこのメッセージを LOG_ERROR で出す(ピンポイント修正)。
constexpr const char *kZombieLimitReachedMessage =
	"バックエンドの応答不能が繰り返されています。OBS の再起動を推奨します";

// 1024 frames(OBS 内部レート)をデバイスレートへ換算した frames 数(切り上げ)。RingBuffer の
// max_chunk_frames とプリロール計算の両方で使う(レビュー指摘 #2/#3)。
uint32_t FramesPerChunkForDeviceRate(uint32_t device_rate, uint32_t obs_rate)
{
	return (uint32_t)std::ceil((double)kObsFramesPerCallback * (double)device_rate / (double)obs_rate);
}

// config.json の全設定キーを 1 回の読み込みで取得する。Slice 7: 実際の obs_data 読み込み・値域
// クランプ・schema_version 判定は plugin_config::Load()(src/config/plugin-config.{hpp,cpp})へ
// 委譲した。ここでは output_instance::ConfigValues(enabled/schema_version/backend/output_mode を
// 持たないランタイム用の縮小版)への詰め替えのみを行う。Slice 6 から: control thread からのみ呼ぶ
// (UI スレッドの GetEffectiveConfig() は変更なしで呼んでよい。ファイル I/O 自体は S6 の「重い処理」
// 規制の対象外。旧来どおり両スレッドから安全に呼べる)。
output_instance::ConfigValues LoadConfig()
{
	const plugin_config::PluginConfig persisted = plugin_config::Load();

	output_instance::ConfigValues config;
	config.audio_track = persisted.audio_track;
	config.device_id = persisted.device_id;
	config.device_name = persisted.device_name;
	config.buffer_preset = persisted.buffer_preset;
	config.buffer_chunks = persisted.buffer_chunks;
	config.auto_reconnect = persisted.auto_reconnect;
	config.log_level = persisted.log_level;
	return config;
}

// Restart()/GetEffectiveConfig() が config.json 由来の値へランタイムオーバーライドを重ねる。
// フィールドが std::nullopt のものは config 側の値をそのまま残す。
void ApplyOverrides(output_instance::ConfigValues &config, const output_instance::ConfigOverrides &overrides)
{
	if (overrides.audio_track)
		config.audio_track = audio_track_map::ClampAudioTrack(*overrides.audio_track);
	if (overrides.device_id)
		config.device_id = *overrides.device_id;
	if (overrides.device_name)
		config.device_name = *overrides.device_name;
	if (overrides.buffer_preset)
		config.buffer_preset = *overrides.buffer_preset;
	if (overrides.buffer_chunks)
		config.buffer_chunks = *overrides.buffer_chunks;
	if (overrides.auto_reconnect)
		config.auto_reconnect = *overrides.auto_reconnect;
	if (overrides.log_level)
		config.log_level = *overrides.log_level;
}

// Restart() が呼ばれるたびに、指定されたフィールドだけを overrides_ へ蓄積する。前回までの Restart()
// で設定した他のフィールドは保持する。Slice 7 から HandleRestartCommand() がこの結果を config.json
// へも書き戻す(OutputInstance::PersistEffectiveConfig())。
void MergeOverrides(output_instance::ConfigOverrides &base, const output_instance::ConfigOverrides &incoming)
{
	if (incoming.audio_track)
		base.audio_track = incoming.audio_track;
	if (incoming.device_id)
		base.device_id = incoming.device_id;
	if (incoming.device_name)
		base.device_name = incoming.device_name;
	if (incoming.buffer_preset)
		base.buffer_preset = incoming.buffer_preset;
	if (incoming.buffer_chunks)
		base.buffer_chunks = incoming.buffer_chunks;
	if (incoming.auto_reconnect)
		base.auto_reconnect = incoming.auto_reconnect;
	if (incoming.log_level)
		base.log_level = incoming.log_level;
}

// log_level 文字列(REQUIREMENTS.md §3)を obs_log() の blog 閾値(util/base.h の LOG_* 数値)へ
// マップして即座に適用する(D-18)。log_level は音声パスに影響しないため、Restart() の Stop→Start
// 合成を経由せず常に即時反映する。control thread から呼ぶ。
void ApplyLogLevelThreshold(const std::string &log_level)
{
	int threshold = LOG_INFO;
	if (log_level == "error") {
		threshold = LOG_ERROR;
	} else if (log_level == "warning") {
		threshold = LOG_WARNING;
	} else if (log_level == "info") {
		threshold = LOG_INFO;
	} else if (log_level == "debug") {
		threshold = LOG_DEBUG;
	} else {
		obs_log(LOG_WARNING, "OutputInstance: unknown log_level=\"%s\"; falling back to \"info\"",
			log_level.c_str());
		threshold = LOG_INFO;
	}
	obs_log_set_level_threshold(threshold);
}

// diagnostics::StartDumpInfo::resolved_by / ログ表示向けの文字列化(D-14)。
const char *ResolvedByToString(device_matcher::ResolvedBy resolved_by)
{
	switch (resolved_by) {
	case device_matcher::ResolvedBy::Default:
		return "default";
	case device_matcher::ResolvedBy::ById:
		return "id";
	case device_matcher::ResolvedBy::ByName:
		return "name";
	}
	return "unknown";
}

// 状態遷移ログ用の文字列化(Slice 6: 状態機械が 5 値になったため、遷移そのものをログで追えるように
// する。AC-20「Recovering 系含む完全判定」の証跡としても有用)。
const char *StateToString(output_instance::State state)
{
	switch (state) {
	case output_instance::State::Stopped:
		return "Stopped";
	case output_instance::State::Starting:
		return "Starting";
	case output_instance::State::Running:
		return "Running";
	case output_instance::State::Recovering:
		return "Recovering";
	case output_instance::State::Failed:
		return "Failed";
	}
	return "Unknown";
}

// FILETIME(100ns 単位、1601 年基準)を単一の uint64_t へ結合する(GetThreadTimes/
// GetSystemTimeAsFileTime の両方で使う共通ヘルパ)。
uint64_t FileTimeTo100Ns(const FILETIME &ft)
{
	return ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
}

// レビュー指摘 Medium-2(アイドル CPU の分解能): 2 秒窓の差分ではなく、control thread の
// GetThreadTimes() creation 以降の累積 CPU 時間 / 経過時間で CPU% を出す(窓長に依存しない値)。
// control thread 自身から呼ぶこと(GetCurrentThread() は呼び出し元スレッド専用の疑似ハンドル)。
// 取得に失敗したら 0.0 を返す。
double ComputeControlThreadCumulativeCpuPercent()
{
	FILETIME creation_time{}, exit_time{}, kernel_time{}, user_time{};
	if (!GetThreadTimes(GetCurrentThread(), &creation_time, &exit_time, &kernel_time, &user_time))
		return 0.0;

	FILETIME now_ft{};
	GetSystemTimeAsFileTime(&now_ft);

	const uint64_t cpu_100ns = FileTimeTo100Ns(kernel_time) + FileTimeTo100Ns(user_time);
	const uint64_t creation_100ns = FileTimeTo100Ns(creation_time);
	const uint64_t now_100ns = FileTimeTo100Ns(now_ft);
	if (now_100ns <= creation_100ns)
		return 0.0;

	const uint64_t elapsed_100ns = now_100ns - creation_100ns;
	return (double)cpu_100ns / (double)elapsed_100ns * 100.0;
}

// IMMDeviceEnumerator::GetDevice() は wide 文字列を要求する(wasapi-output.cpp の Utf8ToWide() と
// 同じ変換。レビュー指摘 Medium-5: DeviceArrived の flow 判定用に control thread 側でも使う)。
std::wstring Utf8ToWide(const std::string &utf8)
{
	if (utf8.empty())
		return std::wstring();
	const int wide_len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	if (wide_len <= 1)
		return std::wstring();
	std::wstring wide(static_cast<size_t>(wide_len - 1), L'\0'); // wide_len には終端 NUL 分を含む
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), wide_len);
	return wide;
}

} // namespace

OutputInstance::OutputInstance()
{
	// backend_ は control thread が唯一の書き手だが、構築中(コンストラクタ実行中)は他スレッドから
	// 見えないため ui_mutex_ なしで backend_ptr_ を初期化してよい。
	backend_ = std::make_unique<WasapiOutput>();
	backend_ptr_ = backend_.get();
	// レビュー指摘(High-3): EXIT 時に render thread ハンドシェイク待ちを短縮させるため、backend_ を
	// 構築するたびに shutting_down_ を共有する(コンストラクタ実行中はまだ他スレッドから
	// shutting_down_ へ書き込まれ得ないため、ここでの読み取りも安全)。
	backend_->SetShutdownFlag(&shutting_down_);
}

OutputInstance::~OutputInstance()
{
	// 保険(通常は plugin-main.cpp の OBS_FRONTEND_EVENT_EXIT で Shutdown() 済み)。Shutdown() は
	// 冪等なので二重に呼んでも安全(qt-ui-and-lifecycle.md: obs_module_unload の冪等な保険と同じ考え方)。
	Shutdown();
}

void OutputInstance::StartControlThread()
{
	bool expected = false;
	if (!control_thread_started_.compare_exchange_strong(expected, true))
		return; // 二重呼び出しは no-op。
	control_thread_ = std::thread(&OutputInstance::ControlThreadMain, this);
}

void OutputInstance::Shutdown()
{
	if (!control_thread_started_.load(std::memory_order_acquire))
		return; // StartControlThread() が呼ばれていない。
	if (!control_thread_.joinable())
		return; // 既に Shutdown 済み(冪等)。

	// レビュー指摘(High-3): 冒頭で立てる。control thread が AttemptConnect() の途中(デバイス列挙/
	// WasapiOutput::Open/Start の cv 待ち含む)にいても、各フェーズ境界・cv 待ちが早期に中断されて
	// 以下の Stop/Shutdown コマンドの処理へ進めるようになる(目標: 通常 <1s、最悪でも ~3s)。
	shutting_down_.store(true, std::memory_order_release);

	// ブリーフ実装内容: Dock タイマー停止(呼び出し元の plugin-main.cpp が先に行う)→ control thread へ
	// Stop 要求 → control thread join。Stop を先に enqueue しておくことで、Shutdown コマンドが処理
	// される時点では既に TeardownActiveSession() 済み(FIFO で順序が保証される)。CanAcceptCommands()
	// を経由しない直接 Push(): Shutdown() 自身は shutting_down_==true でも必ずコマンドを積む必要がある。
	command_queue_.Push(output_instance::ControlCommand{output_instance::ControlCommandType::Stop, {}});
	command_queue_.Push(output_instance::ControlCommand{output_instance::ControlCommandType::Shutdown, {}});
	control_thread_.join();
}

bool OutputInstance::CanAcceptCommands() const
{
	// レビュー指摘(Medium-4): StartControlThread() 未呼び出し、または Shutdown() 開始後にコマンドを
	// 積んでも、control thread が存在しない/もう処理を続けないため無言で握りつぶされる
	// (キューに残ったまま誰にも読まれない)。呼び出し元へ気づけるよう LOG_WARNING を 1 回出す。
	if (!control_thread_started_.load(std::memory_order_acquire)) {
		obs_log(LOG_WARNING, "OutputInstance: command ignored; StartControlThread() has not been called yet");
		return false;
	}
	if (shutting_down_.load(std::memory_order_acquire)) {
		obs_log(LOG_WARNING, "OutputInstance: command ignored; shutdown already in progress");
		return false;
	}
	return true;
}

void OutputInstance::Start()
{
	if (!CanAcceptCommands())
		return;
	command_queue_.Push(output_instance::ControlCommand{output_instance::ControlCommandType::Start, {}});
}

void OutputInstance::Stop()
{
	if (!CanAcceptCommands())
		return;

	// Slice 7(実装内容の要点 1b、REQ-CONFIG-001、AC-08 の根拠): public Stop() は Dock の
	// 停止/キャンセルボタンと Tools メニューからしか呼ばれない(EXIT シーケンスは Shutdown() が
	// Stop コマンドを command_queue_ へ直接 push するためここを通らない)。つまりここへ来る呼び出しは
	// 常にユーザーの明示的な「止める」意思なので、enabled=false を同期的に(UI/Qt Tools メニュー
	// スレッド上、ファイル I/O のみで RT ではない)保存してから Stop コマンドを enqueue する。
	PersistEffectiveConfig(false, GetEffectiveConfig());

	command_queue_.Push(output_instance::ControlCommand{output_instance::ControlCommandType::Stop, {}});
}

void OutputInstance::Restart(const output_instance::ConfigOverrides &overrides)
{
	if (!CanAcceptCommands())
		return;
	output_instance::ControlCommand cmd;
	cmd.type = output_instance::ControlCommandType::Restart;
	cmd.overrides = overrides;
	command_queue_.Push(std::move(cmd));
}

void OutputInstance::RequestReregister()
{
	if (!CanAcceptCommands())
		return;
	command_queue_.Push(output_instance::ControlCommand{output_instance::ControlCommandType::Reregister, {}});
}

output_instance::ConfigValues OutputInstance::GetEffectiveConfig() const
{
	output_instance::ConfigOverrides overrides_copy;
	{
		std::lock_guard<std::mutex> lock(ui_mutex_);
		overrides_copy = overrides_;
	}
	output_instance::ConfigValues config = LoadConfig();
	ApplyOverrides(config, overrides_copy);
	return config;
}

float OutputInstance::LevelPeakL() const
{
	std::lock_guard<std::mutex> lock(ui_mutex_);
	return backend_ptr_ ? backend_ptr_->LevelPeakL() : 0.0f;
}

float OutputInstance::LevelPeakR() const
{
	std::lock_guard<std::mutex> lock(ui_mutex_);
	return backend_ptr_ ? backend_ptr_->LevelPeakR() : 0.0f;
}

diagnostics::DiagnosticsSnapshot OutputInstance::BuildDiagnosticsSnapshot() const
{
	// Slice 9: config_persist_mutex_(ファイル I/O)を先に読み終えてから ui_mutex_ を取る(逐次、
	// ネストしない)。二重ロックの順序を作らないための意図的な組み立て順(config_persist_mutex_ →
	// ui_mutex_ の入れ子は他に存在しないため本来は安全だが、将来の変更で誤って入れ子にしないよう
	// ここでも明示的に「先に読んで解放してから」の形にする)。
	const std::string output_mode = GetPersistedOutputMode();

	// ui_mutex_ は「保持オブジェクトを差し替える一瞬」だけ保持するものであり、backend_ptr_/
	// ring_buffer_ptr_ が指す先の破棄は必ずこのロックの外(かつロック解放後)で行われる(S6-1、
	// CleanupFailedAttempt() 参照)。そのため、このロックを保持したまま backend_ptr_/ring_buffer_ptr_
	// を読んでそのままメソッド呼び出しまで行っても UAF にならない。
	std::lock_guard<std::mutex> lock(ui_mutex_);

	diagnostics::DiagnosticsSnapshot snapshot;
	snapshot.plugin_version = PLUGIN_VERSION;
	snapshot.obs_version = obs_get_version_string();
	snapshot.os_version = diagnostics::GetOsVersionString();
	snapshot.device_id = selected_device_id_;
	snapshot.device_name = selected_device_name_;
	snapshot.obs_sample_rate = last_obs_sample_rate_;
	snapshot.output_sample_rate = last_output_sample_rate_;
	snapshot.channel_count = last_channel_count_;
	snapshot.buffer_chunks = last_buffer_chunks_;
	snapshot.buffer_chunks_requested = last_buffer_chunks_requested_;
	snapshot.backend_name = "wasapi";
	// Slice 6: Recovering→Running の成功のたびに増える実カウント(IncrementReconnectEvents() 参照)。
	snapshot.reconnect_events = reconnect_events_;
	snapshot.underrun_count = backend_ptr_ ? backend_ptr_->UnderrunCount() : 0;
	snapshot.overrun_count = ring_buffer_ptr_ ? ring_buffer_ptr_->OverrunCount() : 0;
	snapshot.clip_count = backend_ptr_ ? backend_ptr_->ClipCount() : 0;
	snapshot.init_error = last_init_error_;
	// Slice 9(REQ-COMPAT-001): AC-22 の 15 項目とは別枠の追加情報(既存 15 項目の意味・順序は不変)。
	snapshot.output_mode = output_mode;
	snapshot.compat_apply_count = compat_apply_count_;
	snapshot.compat_restore_count = compat_restore_count_;
	snapshot.compat_reapply_count = compat_reapply_count_;
	return snapshot;
}

void OutputInstance::SetInitError(const std::string &message)
{
	std::lock_guard<std::mutex> lock(ui_mutex_);
	last_init_error_ = message;
}

void OutputInstance::IncrementReconnectEvents()
{
	std::lock_guard<std::mutex> lock(ui_mutex_);
	++reconnect_events_;
}

std::string OutputInstance::GetPersistedOutputMode() const
{
	std::lock_guard<std::mutex> lock(config_persist_mutex_);
	// レビュー指摘(Medium-4): BuildDiagnosticsSnapshot() が毎回このメソッド経由で config.json を
	// 読み直していた(診断コピー・ログのたびにディスク I/O)。output_mode の唯一の書き手は
	// SetPersistedOutputMode() なので、初回だけディスクから読み、以後はメモリキャッシュを返す。
	if (!output_mode_cache_initialized_) {
		output_mode_cache_ = plugin_config::Load().output_mode;
		output_mode_cache_initialized_ = true;
	}
	return output_mode_cache_;
}

bool OutputInstance::SetPersistedOutputMode(const std::string &output_mode)
{
	std::lock_guard<std::mutex> lock(config_persist_mutex_);
	plugin_config::PluginConfig persisted = plugin_config::Load();
	persisted.output_mode = output_mode;
	const bool ok = plugin_config::Save(persisted);
	if (ok) {
		// レビュー指摘(Medium-4): キャッシュを書き込みと同時に更新する。
		output_mode_cache_ = output_mode;
		output_mode_cache_initialized_ = true;
	} else {
		obs_log(LOG_WARNING, "OutputInstance: failed to save config.json (output_mode=\"%s\")",
			output_mode.c_str());
	}
	return ok;
}

bool OutputInstance::SetPersistedEnabled(bool enabled)
{
	// レビュー指摘(High-1): 互換モードの有効化/無効化(MonitorCompatController::Enable()/Disable()
	// の成否)は、独立出力の Start 成功時/Stop() 時と同じ「ユーザー意思のみ」規約で enabled を
	// 永続化する。ExternalOutputDock が compat_controller_.Enable()/Disable() の呼び出しに続けて
	// 呼ぶ。
	std::lock_guard<std::mutex> lock(config_persist_mutex_);
	plugin_config::PluginConfig persisted = plugin_config::Load();
	persisted.enabled = enabled;
	const bool ok = plugin_config::Save(persisted);
	if (!ok) {
		obs_log(LOG_WARNING, "OutputInstance: failed to save config.json (enabled=%s)",
			enabled ? "true" : "false");
	}
	return ok;
}

void OutputInstance::SetCompatEventCounts(uint64_t apply_count, uint64_t restore_count, uint64_t reapply_count)
{
	std::lock_guard<std::mutex> lock(ui_mutex_);
	compat_apply_count_ = apply_count;
	compat_restore_count_ = restore_count;
	compat_reapply_count_ = reapply_count;
}

void OutputInstance::PersistEffectiveConfig(std::optional<bool> enabled, const output_instance::ConfigValues &config)
{
	// control thread と UI/Qt Tools メニューのイベントスレッドの両方から呼ばれ得るため、
	// Load()→Save() の read-modify-write 全体を直列化する(ヘッダの config_persist_mutex_ コメント
	// 参照。lost update の防止。ファイル I/O のみで RT ではないため mutex 保持のままでよい)。
	std::lock_guard<std::mutex> io_lock(config_persist_mutex_);

	// enabled を変更しない呼び出し(HandleRestartCommand())でも、ディスク上の他フィールド
	// (schema_version/backend/output_mode 含む)を上書きしてしまわないよう、まず現在の永続値を
	// 読み直してから該当フィールドだけを更新する(read-modify-write)。
	plugin_config::PluginConfig persisted = plugin_config::Load();
	if (enabled)
		persisted.enabled = *enabled;
	persisted.audio_track = config.audio_track;
	persisted.device_id = config.device_id;
	persisted.device_name = config.device_name;
	persisted.buffer_preset = config.buffer_preset;
	persisted.buffer_chunks = config.buffer_chunks;
	persisted.auto_reconnect = config.auto_reconnect;
	persisted.log_level = config.log_level;

	if (!plugin_config::Save(persisted)) {
		obs_log(LOG_WARNING,
			"OutputInstance: failed to save config.json (enabled=%s audio_track=%d device_id=\"%s\")",
			persisted.enabled ? "true" : "false", persisted.audio_track, persisted.device_id.c_str());
	} else {
		// Slice 8(S7 持ち越しの衛生、追跡性): 保存成功も 1 行残す。失敗時と対になる明示ログで、
		// 「変更は自動的に保存されます」(Dock の AutoSaveNote)が実際にいつ効いたかを追える。
		obs_log(LOG_INFO,
			"OutputInstance: config.json saved (enabled=%s audio_track=%d device_id=\"%s\" "
			"buffer_preset=\"%s\" auto_reconnect=%s log_level=\"%s\")",
			persisted.enabled ? "true" : "false", persisted.audio_track, persisted.device_id.c_str(),
			persisted.buffer_preset.c_str(), persisted.auto_reconnect ? "true" : "false",
			persisted.log_level.c_str());
	}
}

void OutputInstance::TransitionTo(output_instance::State new_state)
{
	const output_instance::State previous = state_.load(std::memory_order_acquire);
	state_.store(new_state, std::memory_order_release);
	if (previous != new_state) {
		obs_log(LOG_INFO, "OutputInstance: state %s -> %s", StateToString(previous), StateToString(new_state));
	}
	// レビュー指摘(Medium-5): 新しい Recovering セッションに入るたびに、flow 判定不能イベントの
	// フォールバックリセット回数をゼロへ戻す(前回の Recovering セッションの回数を持ち越さない)。
	if (new_state == output_instance::State::Recovering)
		arrival_reset_fallback_count_ = 0;
}

// ============================================================================
// control thread 本体
// ============================================================================

void OutputInstance::ControlThreadMain()
{
	// control thread: COM を MTA で初期化する(ブリーフ実装内容 1: 「COM は MTA 初期化」)。
	// RPC_E_CHANGED_MODE は「既に別モデルで初期化済み」を意味し利用可能(wasapi-com.md と同じ方針。
	// この control thread は専用スレッドのため通常は発生しないが、念のため同じガードを踏襲する)。
	const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool co_owned = SUCCEEDED(co_hr);
	const bool co_usable = SUCCEEDED(co_hr) || co_hr == RPC_E_CHANGED_MODE;

	if (co_usable) {
		const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
						    __uuidof(IMMDeviceEnumerator),
						    (void **)notify_enumerator_.GetAddressOf());
		if (SUCCEEDED(hr)) {
			// 旧#22: DeviceNotifier は生成時に ref_=1。Register() は AddRef しない。
			device_notifier_ = new DeviceNotifier(&device_events_);
			if (!device_notifier_->Register(notify_enumerator_.Get())) {
				obs_log(LOG_WARNING,
					"OutputInstance: DeviceNotifier::Register failed; automatic default-device "
					"follow-up (AC-17) and device-arrival fast retry will not work this session");
				device_notifier_->Release();
				device_notifier_ = nullptr;
			}
		} else {
			obs_log(LOG_WARNING,
				"OutputInstance: CoCreateInstance(MMDeviceEnumerator) failed on control thread "
				"(hr=0x%08lX); device change notifications unavailable this session",
				hr);
		}
	} else {
		obs_log(LOG_ERROR, "OutputInstance: control thread CoInitializeEx failed (hr=0x%08lX)", co_hr);
	}

	for (;;) {
		std::optional<output_instance::ControlCommand> cmd = command_queue_.WaitPop(kControlLoopPollTimeout);
		if (cmd) {
			HandleCommand(*cmd);
			if (cmd->type == output_instance::ControlCommandType::Shutdown)
				break;
		}

		// IMMNotificationClient からのイベントをまとめて drain する(コールバック自体は enqueue の
		// み、実処理はここで行う。旧#22 準拠)。
		for (;;) {
			std::optional<device_notifier::Event> event = device_events_.TryPop();
			if (!event)
				break;
			HandleDeviceEvent(*event);
		}

		RunPeriodicTasks();
	}

	// レビュー指摘(Low-11): ControlQueue::DrainAll() を実際の EXIT 経路で使う。CanAcceptCommands()
	// (Medium-4)が shutting_down_ 以降の新規 enqueue を拒否するため、通常この時点で command_queue_ は
	// 空のはずだが、万一取りこぼしたコマンドが残っていれば診断のため警告する。
	const std::vector<output_instance::ControlCommand> leftover_commands = command_queue_.DrainAll();
	if (!leftover_commands.empty()) {
		obs_log(LOG_WARNING, "OutputInstance: %zu control command(s) left unprocessed at EXIT",
			leftover_commands.size());
	}
	// このキューはもう誰も Push/WaitPop しない(control thread はここでループを抜けている)。
	// ControlQueue::Shutdown()/IsShutdown() を実際に使う(現状これを参照する箇所はないが、EXIT で
	// キューの役目が終わったことを明示する。WaitPop 側は既にループを抜けているため、この
	// notify_all() で誰かが起こされることはない)。
	command_queue_.Shutdown();

	// EXIT: Shutdown() が Stop コマンドを先に enqueue しているため、ここに来る時点で backend_/
	// mix_source_/diag_logger_ は既に停止済み(TeardownActiveSession() 済み)。IMMNotificationClient
	// の登録解除は control thread 上でのみ許される(旧#22)。
	if (device_notifier_) {
		// レビュー指摘(High-2a): Unregister() の「前」に disabled_ を立てる。Unregister() 呼び出しと
		// in-flight のコールバックが競合しても、以後(および競合中)のコールバックはすべて早期
		// return するため、これから delete しない device_notifier_ へ触れ続けても安全になる。
		device_notifier_->Disable();
		device_notifier_->Unregister(notify_enumerator_.Get());
		// レビュー指摘(High-2b): Unregister() 後も Release() で解放しない(意図的リーク)。
		// Unregister() が「呼び出し中の他スレッドのコールバックの完了を待つ」保証は MS Learn/OBS
		// 実装からは確認できていない(未確認事項)。delete して解放した直後に in-flight の
		// コールバックが this へ触れると UAF になり得るため、zombie_backends_ と同じ方針で
		// プロセス終了まで生かし続ける(Disable() 済みなので実害はコールバック 1 回分の早期
		// return のみ)。
		device_notifier_ = nullptr;
	}
	notify_enumerator_.Reset();

	if (co_owned)
		CoUninitialize();
}

void OutputInstance::HandleCommand(const output_instance::ControlCommand &cmd)
{
	switch (cmd.type) {
	case output_instance::ControlCommandType::Start:
		HandleStartCommand();
		break;
	case output_instance::ControlCommandType::Stop:
		if (state_.load(std::memory_order_acquire) != output_instance::State::Stopped) {
			TeardownActiveSession();
			TransitionTo(output_instance::State::Stopped);
		}
		break;
	case output_instance::ControlCommandType::Restart:
		HandleRestartCommand(cmd.overrides);
		break;
	case output_instance::ControlCommandType::Reregister:
		HandleReregisterCommand();
		break;
	case output_instance::ControlCommandType::Shutdown:
		// 保険(通常は直前の Stop コマンドで既に Stopped のはず。Shutdown() 参照)。
		if (state_.load(std::memory_order_acquire) != output_instance::State::Stopped) {
			TeardownActiveSession();
			TransitionTo(output_instance::State::Stopped);
		}
		break;
	}
}

void OutputInstance::HandleStartCommand()
{
	const output_instance::State current = state_.load(std::memory_order_acquire);
	if (current == output_instance::State::Starting || current == output_instance::State::Running ||
	    current == output_instance::State::Recovering)
		return; // 既に動作中/再接続中。

	// Slice 9(REQ-COMPAT-001 制約5、排他): 互換モード中は独立出力を起動しない。状態は Stopped の
	// ままにする(slice-9.md 実装内容の要点 4)。GetPersistedOutputMode() は config.json を読むだけ
	// (RT ではないファイル I/O、control thread から毎回呼んでも Start 試行の頻度は低いため許容)。
	if (GetPersistedOutputMode() == "obs_monitor_compat") {
		obs_log(LOG_WARNING, "OutputInstance: Start ignored; output_mode=\"obs_monitor_compat\" is active "
				     "(REQ-COMPAT-001 制約5, mutual exclusion)");
		return;
	}

	// ピンポイント修正: ゾンビ累積上限(3 個)に達していれば、手動「再接続」= Start 要求でも
	// 新バックエンドでの継続を行わず Failed 固定のままにする。
	if (zombie_limit_reached_) {
		obs_log(LOG_ERROR, "OutputInstance: %s", kZombieLimitReachedMessage);
		TransitionTo(output_instance::State::Failed);
		return;
	}

	// 手動 Start()(初回、または Failed からの「再接続」ボタン)は常に「新しい試み」として扱う
	// (バックオフの連続失敗カウントをリセットする)。
	reconnect_policy_.Reset();
	TransitionTo(output_instance::State::Starting);

	output_instance::ConfigValues config = LoadConfig();
	{
		std::lock_guard<std::mutex> lock(ui_mutex_);
		ApplyOverrides(config, overrides_);
	}
	ApplyLogLevelThreshold(config.log_level);

	reconnect_device_id_ = config.device_id;
	reconnect_device_name_ = config.device_name;

	if (AttemptConnect(config)) {
		TransitionTo(output_instance::State::Running);
		reconnect_policy_.Reset();

		// Slice 7(実装内容の要点 1a/1d、REQ-CONFIG-001/002、D-14): Start 成功のたびに enabled=true と
		// 実効設定一式を config.json へ保存する。device_id/device_name は config.device_id ではなく
		// reconnect_device_id_/reconnect_device_name_ を使う: AttemptConnect() が
		// resolved_by==ByName で解決した場合これらは解決済みの新しい id/name に更新済みであり
		// (D-14 の「id 更新保存」)、resolved_by==Default のときは "default" のまま据え置かれる
		// (既定デバイス追従を維持するため、具体的な GUID には固定しない)。この保存はここ 1 箇所だけ
		// なので、FINISHED_LOADING の自動開始・HandleRestartCommand()/RunWatchdog()/
		// HandleReregisterCommand() 経由の内部再構築(いずれも HandleStartCommand() を再入する)でも
		// 同じ経路で保存される。値は既に true/一致しているため再保存は冪等(自動遷移の Failed では
		// この分岐を通らないため書き換わらない)。
		output_instance::ConfigValues persisted_config = config;
		persisted_config.device_id = reconnect_device_id_;
		persisted_config.device_name = reconnect_device_name_;
		PersistEffectiveConfig(true, persisted_config);
	} else {
		// 設計判断(要人間確認、報告の「未確認・懸念」参照): 初回 Starting の失敗は Recovering を
		// 経由せず直接 Failed へ遷移する。ブリーフ実装内容 3/4 の Recovering は「Running 中の
		// render thread 失敗」からの遷移として記述されており、AC-05/06/18/23 の完了条件も
		// 「切断/再接続」(=いったん Running に到達した後の話)を扱っている。この解釈のもと、
		// デバイスが最初から存在しない/誤設定のケースは Starting→Failed のまま(手動「再接続」で
		// 再試行)とし、REQUIREMENTS.md §4 の正本定義とはメイン側で突き合わせてもらう。
		TransitionTo(output_instance::State::Failed);
	}
}

void OutputInstance::HandleRestartCommand(const output_instance::ConfigOverrides &incoming)
{
	// 「実効値が現在と同一なら no-op」(旧 Restart() のレビュー指摘 H-1 を踏襲)。overrides_ を
	// 書き換える前の実効値(before)と書き換えた後の実効値(after)を比較し、実際に再起動対象の値が
	// 変わったときだけ Stop→Start 相当を行う。
	output_instance::ConfigValues before = LoadConfig();
	{
		std::lock_guard<std::mutex> lock(ui_mutex_);
		ApplyOverrides(before, overrides_);
	}

	output_instance::ConfigOverrides merged;
	{
		std::lock_guard<std::mutex> lock(ui_mutex_);
		MergeOverrides(overrides_, incoming);
		merged = overrides_;
	}

	// log_level は音声パスに影響しないため、再起動判定を待たず常に即時反映する。
	if (incoming.log_level)
		ApplyLogLevelThreshold(*incoming.log_level);

	output_instance::ConfigValues after = LoadConfig();
	ApplyOverrides(after, merged);

	// Slice 7(実装内容の要点 1c、REQ-CONFIG-001): HandleRestartCommand() は Dock の Advanced 設定
	// からのユーザー操作でのみ呼ばれる(OutputInstance::Restart() 以外に呼び出し元は無い)。実際に
	// 再起動が必要か・その結果 Running へ到達できるかに関わらず、ここでオーバーライドの確定を
	// config.json へも保存する(enabled は変更しない。std::nullopt)。
	PersistEffectiveConfig(std::nullopt, after);

	// auto_reconnect は値の保持・表示に加え、Slice 6 で実際に動作へ反映される(切断検知時の遷移先)が、
	// 値を変えただけで出力を再起動する必要はない。
	const bool need_restart = incoming.audio_track.has_value() || incoming.device_id.has_value() ||
				  incoming.device_name.has_value() || incoming.buffer_preset.has_value() ||
				  incoming.buffer_chunks.has_value();
	if (!need_restart)
		return;

	const bool effective_changed = before.audio_track != after.audio_track || before.device_id != after.device_id ||
				       before.device_name != after.device_name ||
				       before.buffer_preset != after.buffer_preset ||
				       ResolveBufferChunks(before) != ResolveBufferChunks(after);
	if (!effective_changed)
		return;

	const output_instance::State current = state_.load(std::memory_order_acquire);
	// レビュー指摘(Low-12): State::Starting はここでは到達不能なので分岐から削除した。control
	// thread は単一スレッドでコマンドを直列処理し、Starting は HandleStartCommand() 自身が
	// AttemptConnect() を同期的に呼んでいる間だけ観測される一時状態(呼び出しが完了する前に
	// 次のコマンド=この HandleRestartCommand() が処理されることは無い)。よって、ここで Restart
	// コマンドを処理している時点の state_ は Stopped/Running/Recovering/Failed のいずれかにしか
	// なり得ない。
	if (current == output_instance::State::Running || current == output_instance::State::Recovering) {
		obs_log(LOG_INFO,
			"OutputInstance: Restart() applying new Advanced settings while %s; stopping and restarting",
			StateToString(current));
		TeardownActiveSession();
		TransitionTo(output_instance::State::Stopped);
		HandleStartCommand();
	}
	// Stopped/Failed 中は次回 Start()(または「再接続」ボタン)で自動的に新設定が使われるだけで良く、
	// ここで出力を起動しない。
}

void OutputInstance::HandleReregisterCommand()
{
	const output_instance::State current = state_.load(std::memory_order_acquire);
	if (current != output_instance::State::Running && current != output_instance::State::Recovering)
		return; // 出力していないときは再登録の意味が無い。

	// レビュー指摘(High-1、Medium-9): PROFILE_CHANGED(obs_reset_audio)で OBS 内部のサンプルレート
	// 自体が変わっていた場合、単純な Reregister() だけでは AttemptConnect() 時点でサイジングした
	// RingBuffer::max_chunk_frames_/プリロール/DriftCompensator が旧レートのままになる
	// (frames_per_chunk = ceil(1024 * device_rate / obs_rate) の比が変わるため)。obs_sample_rate_
	// at_start_ と現在値を比較し、変化していれば完全再構築(TeardownActiveSession() →
	// HandleStartCommand() 相当)に切り替える。
	obs_audio_info audio_info = {};
	const bool got_info = obs_get_audio_info(&audio_info) && audio_info.samples_per_sec > 0;
	if (got_info && audio_info.samples_per_sec != obs_sample_rate_at_start_) {
		obs_log(LOG_INFO,
			"OutputInstance: OBS sample rate changed (%u -> %u) while %s; rebuilding the output "
			"session instead of a plain reregister",
			obs_sample_rate_at_start_, audio_info.samples_per_sec, StateToString(current));
		TeardownActiveSession();
		TransitionTo(output_instance::State::Stopped);
		HandleStartCommand();
		return;
	}

	ReregisterMixSourceNow();
}

void OutputInstance::ReregisterMixSourceNow()
{
	if (device_sample_rate_for_reregister_ == 0)
		return; // まだ一度も Open に成功していない(通常到達しない防御)。
	mix_source_.Reregister(device_sample_rate_for_reregister_);
}

std::optional<bool> OutputInstance::IsRenderDeviceId(const std::string &device_id) const
{
	// レビュー指摘(Medium-5): IMMNotificationClient コールバック自身の中ではなく control thread 上
	// (この関数は HandleDeviceEvent() からのみ呼ばれる)での通常の COM 呼び出しのため、旧#22 の
	// 禁止 4 項目(ノンブロッキング必須・同期待ち禁止・Register/Unregister 再入禁止・MMDevice
	// オブジェクトの最終参照解放禁止、いずれもコールバック実装内が対象)には該当しない。
	if (!notify_enumerator_ || device_id.empty())
		return std::nullopt;

	const std::wstring wide_id = Utf8ToWide(device_id);
	if (wide_id.empty())
		return std::nullopt;

	Microsoft::WRL::ComPtr<IMMDevice> device;
	if (FAILED(notify_enumerator_->GetDevice(wide_id.c_str(), device.GetAddressOf())))
		return std::nullopt; // 判定不能(切断直後の競合等で GetDevice が失敗するケースを含む)。

	Microsoft::WRL::ComPtr<IMMEndpoint> endpoint;
	if (FAILED(device.As(&endpoint)))
		return std::nullopt;

	EDataFlow flow = eRender;
	if (FAILED(endpoint->GetDataFlow(&flow)))
		return std::nullopt;

	return flow == eRender;
}

void OutputInstance::HandleDeviceEvent(const device_notifier::Event &event)
{
	// レビュー指摘(High-2b): イベント種別を問わず増分する(MonitorCompatController の自己無音警告
	// デバイス列挙キャッシュの無効化トリガ。過剰無効化は安全側 = 許容する)。
	device_topology_change_count_.fetch_add(1, std::memory_order_release);

	switch (event.type) {
	case device_notifier::EventType::DefaultDeviceChanged:
		// 200ms デバウンス: 新しいイベントが来るたびに発火時刻を延長する(トレーリングデバウンス。
		// wasapi-com.md/ブリーフ実装内容 7)。
		default_device_change_pending_ = true;
		default_device_change_fire_ns_ = os_gettime_ns() + kDefaultDeviceChangeDebounceNs;
		break;
	case device_notifier::EventType::DeviceArrived: {
		// Recovering 中ならバックオフをリセットして即試行する(ブリーフ実装内容 8)。
		if (state_.load(std::memory_order_acquire) != output_instance::State::Recovering)
			break;

		// レビュー指摘(Medium-5): 入力(eCapture)デバイスの到着イベントで誤ってバックオフを
		// リセットしないよう eRender のみに反応する。加えて、reconnect_device_id_ が具体的な id
		// (default 以外)のときは、その id に関係するイベントだけをトリガにする(default のときは
		// 「その時点の既定デバイス」を毎回解決し直すため、どの eRender 到着でも意味がある)。
		const bool id_matches = reconnect_device_id_ == "default" || event.device_id == reconnect_device_id_;
		const std::optional<bool> is_render = IsRenderDeviceId(event.device_id);

		if (is_render.has_value()) {
			if (!*is_render || !id_matches)
				break; // 入力デバイス、または無関係な id: 反応しない。
		} else {
			// 判定不能(GetDevice/QueryInterface 失敗)。誤検知の可能性を残したまま無制限に
			// リセットし続けると、切断直後に無関係なイベントが連発した場合バックオフが実質
			// 機能しなくなるため、Recovering セッションあたり連続 kMaxArrivalResetFallback 回
			// までのみリセットを許容する(以後は通常のバックオフに委ねる)。
			if (arrival_reset_fallback_count_ >= kMaxArrivalResetFallback)
				break;
			++arrival_reset_fallback_count_;
		}

		reconnect_policy_.Reset();
		next_reconnect_attempt_ns_ = 0;
		obs_log(LOG_INFO,
			"OutputInstance: device arrival detected while Recovering; retrying immediately (id=%s)",
			event.device_id.c_str());
		break;
	}
	}
}

void OutputInstance::RunPeriodicTasks()
{
	const uint64_t now_ns = os_gettime_ns();

	// Running 中に render thread が回復不能なエラーで停止していないか検査する(ブリーフ実装内容 3)。
	if (state_.load(std::memory_order_acquire) == output_instance::State::Running && backend_ &&
	    backend_->HasFailed()) {
		obs_log(LOG_ERROR, "OutputInstance: backend reported an unrecoverable failure while Running");

		output_instance::ConfigValues config = LoadConfig();
		{
			std::lock_guard<std::mutex> lock(ui_mutex_);
			ApplyOverrides(config, overrides_);
		}

		CleanupFailedAttempt();

		// ピンポイント修正: ゾンビ累積上限に達していれば auto_reconnect の値によらず Failed 固定
		// (新バックエンドでの継続を止める)。
		if (config.auto_reconnect && !zombie_limit_reached_) {
			reconnect_policy_.Reset();
			next_reconnect_attempt_ns_ = 0; // 即座に 1 回目を試みる。
			TransitionTo(output_instance::State::Recovering);
		} else {
			// ブリーフ実装内容 5: auto_reconnect=false なら切断検知で直接 Failed。
			TransitionTo(output_instance::State::Failed);
		}
	}

	RunWatchdog(now_ns);
	RunReconnectTick(now_ns);
	RunDebouncedDefaultDeviceChange(now_ns);
	// Slice 8(AC-12): state_ に関わらず毎ティック呼ぶ(Stopped 中のアイドル CPU も追えるようにする。
	// 関数内部で 2 秒間隔をガードする)。
	SampleControlThreadCpu(now_ns);
	// ピンポイント修正(AC-12): Stopped 中のみ 60 秒ごとに idle stats を 1 行出す。
	LogIdleStatsIfDue(now_ns);
}

void OutputInstance::LogIdleStatsIfDue(uint64_t now_ns)
{
	if (state_.load(std::memory_order_acquire) != output_instance::State::Stopped) {
		// Running/Recovering/Starting/Failed 中は出さない。Stopped から離れたら計測ウィンドウを
		// リセットし、次に Stopped へ戻ったときに即座にログが出ず 60 秒待つようにする(ログスパム
		// 防止。Start/Stop を短時間で繰り返しても連打にならない)。
		idle_stats_last_log_ns_ = 0;
		return;
	}
	if (idle_stats_last_log_ns_ == 0) {
		idle_stats_last_log_ns_ = now_ns; // Stopped に入った/戻った直後: ウィンドウの起点を記録するのみ。
		return;
	}
	if (now_ns - idle_stats_last_log_ns_ < kIdleStatsLogIntervalNs)
		return;
	idle_stats_last_log_ns_ = now_ns;

	// レビュー指摘 Medium-2: 2 秒窓の cpu_control_pct_(Running 中の周期ログと同じ指標)ではなく、
	// control thread の GetThreadTimes() creation 以降の累積 CPU% を出す(フィールド名
	// cpu_control_cum_pct。60 秒間隔で出すこの行を 2 秒窓ベースにすると、直近の 200ms ポーリング
	// ループの瞬間値に引きずられて分解能が粗くなるため)。
	// ピンポイント修正: mix stats と同じ理由(配布環境の既定 info でのログ肥大防止)で DEBUG に変更。
	obs_log(LOG_DEBUG, "idle stats: cpu_control_cum_pct=%.3f", ComputeControlThreadCumulativeCpuPercent());
}

void OutputInstance::SampleControlThreadCpu(uint64_t now_ns)
{
	if (now_ns - cpu_sample_last_check_ns_ < kCpuSampleIntervalNs && cpu_sample_has_prev_)
		return;
	cpu_sample_last_check_ns_ = now_ns;

	// control thread は RT ループ(RenderLoop)ではないため、GetThreadTimes を自スレッドから直接
	// 呼んでよい(rt-audio.md の禁止対象は render thread/callback のみ)。GetCurrentThread() は
	// 呼び出し元スレッド専用の疑似ハンドルで、この関数自身が常に control thread から呼ばれる前提
	// (RunPeriodicTasks() 経由)でのみ有効。
	FILETIME creation_time{}, exit_time{}, kernel_time{}, user_time{};
	if (!GetThreadTimes(GetCurrentThread(), &creation_time, &exit_time, &kernel_time, &user_time))
		return;

	const auto to_100ns = [](const FILETIME &ft) -> uint64_t {
		return ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
	};
	const uint64_t cpu_100ns = to_100ns(kernel_time) + to_100ns(user_time);

	// 初回サンプルは差分が取れないため基準値の記録のみ(WasapiOutput::SampleCpuUsage() と同じ
	// ウィンドウ境界の扱い、C-08)。
	if (cpu_sample_has_prev_) {
		const uint64_t wall_delta_ns = now_ns - cpu_sample_prev_wall_ns_;
		if (wall_delta_ns > 0 && cpu_100ns >= cpu_sample_prev_cpu_100ns_) {
			const uint64_t cpu_delta_ns = (cpu_100ns - cpu_sample_prev_cpu_100ns_) * 100ULL;
			const double pct = (double)cpu_delta_ns / (double)wall_delta_ns * 100.0;
			cpu_control_pct_.store((float)pct, std::memory_order_relaxed);
		}
	}
	cpu_sample_prev_cpu_100ns_ = cpu_100ns;
	cpu_sample_prev_wall_ns_ = now_ns;
	cpu_sample_has_prev_ = true;
}

void OutputInstance::RunWatchdog(uint64_t now_ns)
{
	// ブリーフ実装内容 11: Running 時のみ 500ms 周期で callback 進行を監視する。
	if (state_.load(std::memory_order_acquire) != output_instance::State::Running)
		return;
	if (now_ns - watchdog_last_check_ns_ < kWatchdogCheckIntervalNs)
		return;
	watchdog_last_check_ns_ = now_ns;

	uint64_t callbacks = 0;
	mix_source_.GetCumulativeStats(&callbacks, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

	if (callbacks != watchdog_last_callback_count_) {
		watchdog_last_callback_count_ = callbacks;
		watchdog_last_progress_ns_ = now_ns;
		watchdog_warned_ = false;
		return;
	}

	if (now_ns - watchdog_last_progress_ns_ < kWatchdogStallThresholdNs)
		return;

	// 1 秒以上無進行: obs_get_audio_info() を再取得して再登録する。警告は事象あたり 1 回のみ
	// (「復帰しなければ繰り返し(上限なしだが警告は抑制)」)。再登録自体は復帰するまで毎ティック retry する
	// (callback_count_ が変化すれば次のティックで正常検出され watchdog_warned_ もリセットされる)。
	if (!watchdog_warned_) {
		obs_log(LOG_WARNING, "OutputInstance: no raw audio callback progress for >=1s while Running; "
				     "re-fetching obs_get_audio_info() and reregistering (will keep retrying silently "
				     "until progress resumes)");
		watchdog_warned_ = true;
	}

	obs_audio_info audio_info = {};
	if (!obs_get_audio_info(&audio_info) || audio_info.samples_per_sec == 0)
		return;

	// レビュー指摘(High-1): HandleReregisterCommand() と同じガード。ウォッチドッグ経由の再登録でも
	// OBS サンプルレートが変わっていれば単純な Reregister() ではなく完全再構築へ切り替える。
	if (audio_info.samples_per_sec != obs_sample_rate_at_start_) {
		obs_log(LOG_INFO,
			"OutputInstance: OBS sample rate changed (%u -> %u) while stalled and Running; rebuilding "
			"the output session instead of a plain reregister",
			obs_sample_rate_at_start_, audio_info.samples_per_sec);
		TeardownActiveSession();
		TransitionTo(output_instance::State::Stopped);
		HandleStartCommand();
		return;
	}

	ReregisterMixSourceNow();
}

void OutputInstance::RunReconnectTick(uint64_t now_ns)
{
	// レビュー指摘(High-3): EXIT 中は新しい再接続試行(=AttemptConnect() の長い処理)を開始しない。
	if (shutting_down_.load(std::memory_order_acquire))
		return;
	if (state_.load(std::memory_order_acquire) != output_instance::State::Recovering)
		return;
	if (now_ns < next_reconnect_attempt_ns_)
		return;

	output_instance::ConfigValues config = LoadConfig();
	{
		std::lock_guard<std::mutex> lock(ui_mutex_);
		ApplyOverrides(config, overrides_);
	}
	// ブリーフ実装内容 4: 「既定 or 保存 id で再 Open」。config.json を都度読み直しても device_id は
	// 上と同じ値になるはずだが、Restart() 経由でオーバーライドが変わっている可能性もあるため、
	// 直近の Running セッションで実際に使った id(reconnect_device_id_/name_)を優先して使う
	// (保存値と実効値が食い違っていても、直近成功した接続と同じ id で再接続を試みるのが自然なため)。
	config.device_id = reconnect_device_id_;
	config.device_name = reconnect_device_name_;

	if (AttemptConnect(config)) {
		TransitionTo(output_instance::State::Running);
		reconnect_policy_.Reset();
		IncrementReconnectEvents();
		return;
	}

	reconnect_policy_.NotifyFailure();

	// ピンポイント修正: この試行中に ReplaceBackendIfZombified() がゾンビ上限に達していれば、
	// バックオフを続けず直ちに Failed 固定にする(メッセージは 3 個目発生時に出力済み)。
	if (zombie_limit_reached_) {
		TransitionTo(output_instance::State::Failed);
		return;
	}

	if (reconnect_policy_.ShouldGiveUp()) {
		obs_log(LOG_ERROR,
			"OutputInstance: giving up reconnecting after %d consecutive failures; transitioning to Failed",
			reconnect_policy_.ConsecutiveFailures());
		TransitionTo(output_instance::State::Failed);
		return;
	}

	const std::chrono::milliseconds delay = reconnect_policy_.NextDelay();
	next_reconnect_attempt_ns_ =
		now_ns + (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count();
	obs_log(LOG_WARNING, "OutputInstance: reconnect attempt %d/%d failed; retrying in %lld ms",
		reconnect_policy_.ConsecutiveFailures(), ReconnectPolicy::kMaxConsecutiveFailures,
		(long long)delay.count());
}

void OutputInstance::RunDebouncedDefaultDeviceChange(uint64_t now_ns)
{
	if (!default_device_change_pending_ || now_ns < default_device_change_fire_ns_)
		return;
	default_device_change_pending_ = false;

	if (state_.load(std::memory_order_acquire) != output_instance::State::Running)
		return;
	// AC-17: device_id=="default" を選択している場合のみ既定デバイス変更に追従する。
	if (reconnect_device_id_ != "default")
		return;

	// ピンポイント修正: ゾンビ累積上限に達している間は自動再接続経路自体を抑止する(理論上 state_ が
	// Running のままこの上限に達することはない=到達済みなら手動 Start() も Failed 固定になるため
	// Running へ戻れない、が念のため防御的にガードする)。
	if (zombie_limit_reached_)
		return;

	obs_log(LOG_INFO, "OutputInstance: default output device changed while Running with device_id=\"default\"; "
			  "reopening (AC-17)");
	CleanupFailedAttempt();
	reconnect_policy_.Reset();
	next_reconnect_attempt_ns_ = 0; // 通常の Recovering 経路で即座に 1 回目を試みる。
	TransitionTo(output_instance::State::Recovering);
}

// ============================================================================
// 接続試行・後始末(旧 Start()/Stop() の中身)
// ============================================================================

bool OutputInstance::AttemptConnect(const output_instance::ConfigValues &config)
{
	// 前回の部分状態(直前の失敗した Recovering 試行、または Failed→Start 再入)を必ず一掃してから
	// 開通処理へ進む(初回 Starting でも安全な no-op)。
	CleanupFailedAttempt();
	SetInitError("none");

	// レビュー指摘(High-3): EXIT 要求が既に来ていれば、ここから先の重い処理(列挙/Open/プリロール/
	// Start)を一切行わず即座に諦める。CleanupFailedAttempt() 済みで新規リソースはまだ何も確保して
	// いないため、後始末は不要。呼び出し元(HandleStartCommand()/RunReconnectTick())は false を見て
	// 通常の失敗パスを辿り、control thread はすぐ後続の Stop/Shutdown コマンド処理へ進める。
	if (shutting_down_.load(std::memory_order_acquire)) {
		SetInitError("shutdown in progress");
		return false;
	}

	if (!backend_) {
		backend_ = std::make_unique<WasapiOutput>();
		backend_->SetShutdownFlag(&shutting_down_);
	}

	// デバイス列挙(control thread 上。もう UI/frontend イベントスレッドをブロックしない、S6 の主目的)。
	const uint64_t enumerate_start_ns = os_gettime_ns();
	const AudioDeviceEnumeration enumeration = AudioDeviceManager::Enumerate();
	const uint64_t enumerate_elapsed_ms = (os_gettime_ns() - enumerate_start_ns) / 1000000ULL;
	constexpr uint64_t kEnumerateWarnThresholdMs = 500;
	if (enumerate_elapsed_ms > kEnumerateWarnThresholdMs) {
		obs_log(LOG_WARNING, "OutputInstance: device enumeration took %llu ms (>%llu ms) on the control thread",
			(unsigned long long)enumerate_elapsed_ms, (unsigned long long)kEnumerateWarnThresholdMs);
	}

	if (!enumeration.succeeded) {
		obs_log(LOG_ERROR, "OutputInstance: device enumeration API failed (hr=0x%08lX); this may be transient",
			(unsigned long)enumeration.enumeration_hr);
		char err_buf[96];
		snprintf(err_buf, sizeof(err_buf), "device enumeration failed (hr=0x%08lX)",
			 (unsigned long)enumeration.enumeration_hr);
		SetInitError(err_buf);
		diagnostics::LogDiagnosticsSnapshot(BuildDiagnosticsSnapshot());
		return false;
	}

	device_matcher::UnresolvedReason unresolved_reason = device_matcher::UnresolvedReason::NoMatch;
	std::size_t ambiguous_match_count = 0;
	const std::optional<device_matcher::ResolveResult> resolved = device_matcher::Resolve(
		config.device_id, config.device_name, enumeration.devices, &unresolved_reason, &ambiguous_match_count);
	if (!resolved) {
		if (unresolved_reason == device_matcher::UnresolvedReason::AmbiguousName) {
			obs_log(LOG_ERROR,
				"OutputInstance: failed to resolve device_id=\"%s\": device_name=\"%s\" matched %zu "
				"devices (ambiguous); run 'External Audio Output: List Devices (Debug)' to find a "
				"valid id",
				config.device_id.c_str(), config.device_name.c_str(), ambiguous_match_count);
		} else {
			obs_log(LOG_ERROR,
				"OutputInstance: failed to resolve device_id=\"%s\" device_name=\"%s\" against %zu "
				"enumerated device(s); run 'External Audio Output: List Devices (Debug)' to find a "
				"valid id",
				config.device_id.c_str(), config.device_name.c_str(), enumeration.devices.size());
		}
		SetInitError(unresolved_reason == device_matcher::UnresolvedReason::AmbiguousName
				     ? "device resolution failed: device_name matched multiple devices (ambiguous)"
				     : "device resolution failed: no matching device_id/device_name");
		diagnostics::LogDiagnosticsSnapshot(BuildDiagnosticsSnapshot());
		return false;
	}
	if (resolved->resolved_by == device_matcher::ResolvedBy::ByName) {
		obs_log(LOG_WARNING,
			"OutputInstance: saved device_id=\"%s\" no longer matches any active device; resolved by "
			"device_name=\"%s\" match instead (new id=\"%s\")",
			config.device_id.c_str(), config.device_name.c_str(), resolved->id.c_str());
	}

	std::string selected_device_name = resolved->name;
	std::string selected_device_id = resolved->id;
	if (resolved->resolved_by == device_matcher::ResolvedBy::Default) {
		if (!enumeration.default_device_id.empty()) {
			selected_device_id = enumeration.default_device_id;
			for (const auto &entry : enumeration.devices) {
				if (entry.id == enumeration.default_device_id) {
					selected_device_name = entry.name;
					break;
				}
			}
		}
	}

	// レビュー指摘(High-3): 「Open 前」の中断ポイント。ここまでは列挙/解決のみで新規リソースは
	// 確保していないため、CleanupFailedAttempt() 相当の後始末は不要。
	if (shutting_down_.load(std::memory_order_acquire)) {
		SetInitError("shutdown in progress");
		return false;
	}

	uint32_t device_sample_rate = 0;
	if (!backend_->Open(resolved->id, &device_sample_rate)) {
		obs_log(LOG_ERROR, "OutputInstance: WasapiOutput::Open failed");
		SetInitError("WASAPI Open failed (see log for hr)");
		backend_->Stop(); // 冪等。部分的に確保したリソースの後始末(abandoned_ の join/detach 判定込み)。
		ReplaceBackendIfZombified();
		diagnostics::LogDiagnosticsSnapshot(BuildDiagnosticsSnapshot());
		return false;
	}

	obs_audio_info audio_info = {};
	uint32_t obs_sample_rate = 0;
	if (obs_get_audio_info(&audio_info))
		obs_sample_rate = audio_info.samples_per_sec;
	if (obs_sample_rate == 0) {
		obs_log(LOG_ERROR, "OutputInstance: obs_get_audio_info failed to return a sample rate");
		SetInitError("obs_get_audio_info failed to return a sample rate");
		backend_->Stop();
		ReplaceBackendIfZombified();
		diagnostics::LogDiagnosticsSnapshot(BuildDiagnosticsSnapshot());
		return false;
	}

	const double rate_ratio = (double)device_sample_rate / (double)obs_sample_rate;
	if (rate_ratio > kMaxRateRatio) {
		obs_log(LOG_ERROR,
			"OutputInstance: device/obs sample rate ratio %.3f exceeds limit %.1f (device_rate=%u "
			"obs_rate=%u)",
			rate_ratio, kMaxRateRatio, device_sample_rate, obs_sample_rate);
		char err_buf[96];
		snprintf(err_buf, sizeof(err_buf), "sample rate ratio %.3f exceeds limit %.1f", rate_ratio,
			 kMaxRateRatio);
		SetInitError(err_buf);
		backend_->Stop();
		ReplaceBackendIfZombified();
		diagnostics::LogDiagnosticsSnapshot(BuildDiagnosticsSnapshot());
		return false;
	}

	const uint32_t frames_per_chunk = FramesPerChunkForDeviceRate(device_sample_rate, obs_sample_rate);

	const uint32_t requested_buffer_chunks = ResolveBufferChunks(config);
	const uint32_t preroll_chunks = config.buffer_preset == "low_latency" ? requested_buffer_chunks / 2
									      : (requested_buffer_chunks + 1) / 2;

	uint32_t preroll_frames_total = preroll_chunks * frames_per_chunk;
	if (config.buffer_preset != "low_latency" && preroll_frames_total < backend_->GetBufferFrameCount())
		preroll_frames_total += frames_per_chunk;

	const uint32_t preroll_chunks_needed = (preroll_frames_total + frames_per_chunk - 1) / frames_per_chunk;
	const uint32_t ring_capacity_chunks = requested_buffer_chunks > (preroll_chunks_needed + 1)
						      ? requested_buffer_chunks
						      : (preroll_chunks_needed + 1);

	const uint32_t max_chunk_frames = frames_per_chunk + kMaxChunkFramesMargin;
	ring_buffer_ = std::make_unique<RingBuffer>(ring_capacity_chunks, max_chunk_frames);
	{
		std::lock_guard<std::mutex> lock(ui_mutex_);
		ring_buffer_ptr_ = ring_buffer_.get();
	}
	backend_->BindSource(ring_buffer_.get());

	// レビュー指摘(High-3): 「プリロール前」の中断ポイント。ここまでで Open 済み・ring_buffer_ 確保
	// 済みのため、Start 失敗時と同じ後始末(backend_->Stop()/ゾンビ判定)に加えて ring_buffer_ も
	// 明示的に解放する。
	if (shutting_down_.load(std::memory_order_acquire)) {
		SetInitError("shutdown in progress");
		backend_->Stop();
		ReplaceBackendIfZombified();
		{
			std::lock_guard<std::mutex> lock(ui_mutex_);
			ring_buffer_ptr_ = nullptr;
		}
		ring_buffer_.reset();
		return false;
	}

	uint32_t preroll_frames_pushed = 0;
	for (uint32_t pushed = 0; pushed < preroll_frames_total; pushed += frames_per_chunk) {
		if (!ring_buffer_->PushSilence(frames_per_chunk)) {
			obs_log(LOG_WARNING,
				"OutputInstance: preroll PushSilence failed (pushed=%u/%u frames, "
				"ring_capacity_chunks=%u); preroll may be short",
				pushed, preroll_frames_total, ring_capacity_chunks);
			continue;
		}
		preroll_frames_pushed += frames_per_chunk;
	}

	const double target_level_frames = (double)preroll_frames_pushed;
	const double capacity_frames = (double)ring_buffer_->Capacity() * (double)frames_per_chunk;
	backend_->ConfigureDriftCompensator(target_level_frames, (double)frames_per_chunk, capacity_frames,
					    (double)device_sample_rate);

	mix_source_.SetSink(ring_buffer_.get());
	mix_source_.Start(config.audio_track, device_sample_rate);

	// レビュー指摘(High-3): 「Start 前」の中断ポイント。以降は Start 失敗時と全く同じ後始末で良い。
	if (shutting_down_.load(std::memory_order_acquire)) {
		SetInitError("shutdown in progress");
		mix_source_.Stop();
		backend_->Stop();
		ReplaceBackendIfZombified();
		return false;
	}

	if (!backend_->Start()) {
		obs_log(LOG_ERROR, "OutputInstance: WasapiOutput::Start failed");
		SetInitError("WASAPI Start failed (see log for hr)");
		mix_source_.Stop();
		backend_->Stop();
		ReplaceBackendIfZombified();
		diagnostics::LogDiagnosticsSnapshot(BuildDiagnosticsSnapshot());
		return false;
	}

	ever_running_ = true;
	backend_->EnableUnderrunCounting();

	const WasapiOutput::FormatInfo device_fmt = backend_->GetDeviceFormatInfo();
	const WasapiOutput::FormatInfo stream_fmt = backend_->GetFormatInfo();

	diagnostics::StartDumpInfo dump{};
	dump.device_id = selected_device_id;
	if (resolved->resolved_by == device_matcher::ResolvedBy::Default && !dump.device_id.empty())
		dump.device_id += " (at resolve time)";
	dump.device_name = selected_device_name;
	dump.resolved_by = ResolvedByToString(resolved->resolved_by);
	dump.device_sample_rate = device_fmt.sample_rate;
	dump.device_channels = device_fmt.channels;
	dump.device_bits_per_sample = device_fmt.bits_per_sample;
	dump.device_format_tag = device_fmt.format_tag;
	dump.device_is_float = device_fmt.is_float;
	dump.stream_sample_rate = stream_fmt.sample_rate;
	dump.stream_channels = stream_fmt.channels;
	dump.stream_bits_per_sample = stream_fmt.bits_per_sample;
	dump.stream_format_tag = stream_fmt.format_tag;
	dump.stream_is_float = stream_fmt.is_float;
	dump.obs_sample_rate = obs_sample_rate;
	dump.conversion_sample_rate = device_sample_rate;
	dump.buffer_chunks = ring_buffer_->Capacity();
	dump.buffer_chunks_requested = requested_buffer_chunks;
	dump.backend_name = "wasapi";
	dump.autoconvert_pcm = backend_->AutoConvertUsed();
	diagnostics::LogStartDump(dump);

	{
		std::lock_guard<std::mutex> lock(ui_mutex_);
		selected_device_id_ = dump.device_id;
		selected_device_name_ = dump.device_name;
		last_obs_sample_rate_ = obs_sample_rate;
		last_output_sample_rate_ = stream_fmt.sample_rate;
		last_channel_count_ = stream_fmt.channels;
		last_buffer_chunks_ = ring_buffer_->Capacity();
		last_buffer_chunks_requested_ = requested_buffer_chunks;
		backend_ptr_ = backend_.get();
	}

	diagnostics::LogDiagnosticsSnapshot(BuildDiagnosticsSnapshot());
	// Slice 8(AC-12): cpu_control_pct_ は OutputInstance が RunPeriodicTasks() で state_ に関わらず
	// 継続サンプリングしている値をそのまま渡す(diag_logger_ 側では書き込まない、読むだけ)。
	diag_logger_.Start(&mix_source_, ring_buffer_.get(), backend_.get(), &cpu_control_pct_);

	// Recovering 再接続用に、成功したセッションの device_id/name とデバイスレートを保存する
	// (ブリーフ実装内容 4/10)。resolved_by==Default(config.device_id=="default")のときは
	// reconnect_device_id_ を具体的な GUID(selected_device_id)へ書き換えない: "default" のまま
	// 保持しておけば、次回の再接続(RunReconnectTick)/AC-17 の既定デバイス変更追従でも
	// device_matcher::Resolve() が毎回「その時点の既定デバイス」を新たに解決してくれる
	// (具体的な GUID に固定してしまうと、既定デバイスが変わった後もその GUID を追いかけ続けてしまい、
	// AC-17 の「既定デバイスに追従する」という意図に反する)。ById/ByName のときだけ、解決済みの
	// 具体的な id/name を保存して以後の再接続をそこへ固定する(ブリーフ実装内容 4:「既定 or 保存 id」)。
	if (resolved->resolved_by != device_matcher::ResolvedBy::Default) {
		reconnect_device_id_ = selected_device_id;
		reconnect_device_name_ = selected_device_name;
	}
	device_sample_rate_for_reregister_ = device_sample_rate;
	// レビュー指摘(High-1): HandleReregisterCommand()/RunWatchdog() が「OBS サンプルレートが
	// AttemptConnect() 成功時から変わったか」を判定するための基準値。
	obs_sample_rate_at_start_ = obs_sample_rate;

	// ウォッチドッグの基準をリセットする(新しいセッション開始時点で「進行あり」とみなす)。
	watchdog_last_callback_count_ = 0;
	watchdog_last_progress_ns_ = os_gettime_ns();
	watchdog_warned_ = false;
	watchdog_last_check_ns_ = watchdog_last_progress_ns_;

	return true;
}

void OutputInstance::CleanupFailedAttempt()
{
	// 破棄順序(rt-audio.md): 診断ログの周期スレッドを止める(ring_buffer_/backend_ を破棄する前に
	// 必ず先に止める。UAF 防止)→ AudioMixSource.Stop(callback 解除)→ WasapiOutput 停止/ゾンビ判定。
	diag_logger_.StopThread();
	mix_source_.Stop();
	if (backend_)
		backend_->Stop();
	ReplaceBackendIfZombified();

	// ever_running_(このセグメントで一度でも Running に到達していたか)が true なら、ring_buffer_ を
	// 破棄する前に最終値でサマリを出す(Recovering への遷移も「1 セグメントの終わり」として扱う設計。
	// ヘッダのコメント参照)。ゾンビ化した場合は直前の ReplaceBackendIfZombified() が既にこの
	// サマリを出して ever_running_ を false に戻しているため(レビュー指摘 Low-13)、ここは no-op。
	if (ever_running_) {
		diag_logger_.LogFinalSummary();
		ever_running_ = false;
	}

	// レビュー指摘 Low-9: stop summary で最後の値(cpu_render_pct)を出し終えたので、Stopped 中に
	// 前セッションの値が残り続けないようリセットする(Open() でも次セッション開始時に同様の
	// リセットを行うが、Stop()〜次回 Open() の間の残留を無くす衛生上の措置)。ゾンビ化していれば
	// backend_ は既にリセット済みの新しいインスタンスを指しているため no-op。
	if (backend_)
		backend_->ResetRenderCpuPercent();

	{
		std::lock_guard<std::mutex> lock(ui_mutex_);
		ring_buffer_ptr_ = nullptr;
		backend_ptr_ = backend_.get(); // ゾンビ化していれば新しいインスタンスへ差し替わっている。
	}
	ring_buffer_.reset();
}

void OutputInstance::TeardownActiveSession()
{
	CleanupFailedAttempt();
	// AC-22: 開始ダンプ・停止サマリのいずれかに 15 項目すべてが出ることを保証するため、停止時にも
	// スナップショットを出す(Running→Stop はもちろん、Starting/Recovering/Failed→Stop でも
	// init_error 等を確認できる)。
	diagnostics::LogDiagnosticsSnapshot(BuildDiagnosticsSnapshot());
}

void OutputInstance::ReplaceBackendIfZombified()
{
	if (!backend_ || !backend_->RequiresZombieRetention())
		return;

	obs_log(LOG_ERROR, "OutputInstance: WasapiOutput render thread was unresponsive and had to be detached; "
			   "retaining the instance forever (zombie, S6-1) and continuing with a fresh instance");

	// レビュー指摘(Low-13): まだ backend_ をゾンビ側から新しいインスタンスへ差し替える前(=
	// diag_logger_ が内部に保持しているポインタがまだこのゾンビ側 backend_ を指している)ここで、
	// このセグメントの最終統計を確定的に出し切る。CleanupFailedAttempt() 側の
	// 「if (ever_running_) LogFinalSummary()」に任せると、「新しい backend_ の統計と誤解されない
	// か」がこのメソッドを backend_ 差し替えより先に呼ぶという呼び出し順序に暗黙で依存してしまう
	// ため、ここで明示的に「ゾンビになった側」の最終統計として出し切り、ever_running_ を先に
	// リセットして CleanupFailedAttempt() 側の呼び出しを no-op にする。
	if (ever_running_) {
		diag_logger_.LogFinalSummary();
		ever_running_ = false;
	}

	// 意図的リーク: delete しない。EXIT でも解放しない(見捨てた render thread がまだこのインスタンス
	// を参照している可能性があるため、プロセス終了まで生かし続けることで UAF を根絶する。
	// wasapi-output.hpp の RequiresZombieRetention() コメント参照)。
	zombie_backends_.push_back(backend_.release());

	// ピンポイント修正: ゾンビ累積が上限(3 個)に達したら、以後の自動 Recovering(RunPeriodicTasks/
	// RunReconnectTick/RunDebouncedDefaultDeviceChange)と手動 Start()(「再接続」ボタン、
	// HandleStartCommand() 参照)による新バックエンドでの継続を止める。フラグは一度立てたら戻さない
	// (プロセス寿命の間、Failed 固定)。
	if (!zombie_limit_reached_ && zombie_backends_.size() >= kMaxZombieBackends) {
		zombie_limit_reached_ = true;
		obs_log(LOG_ERROR, "OutputInstance: %s", kZombieLimitReachedMessage);
	}

	backend_ = std::make_unique<WasapiOutput>();
	// レビュー指摘(High-3): 新しく構築した backend_ にも shutting_down_ を共有し直す
	// (WasapiOutput::SetShutdownFlag() は backend_ を構築するたびに呼ぶ規約)。
	backend_->SetShutdownFlag(&shutting_down_);

	std::lock_guard<std::mutex> lock(ui_mutex_);
	backend_ptr_ = backend_.get();
}

uint32_t OutputInstance::ResolveBufferChunks(const output_instance::ConfigValues &config)
{
	if (config.buffer_preset == "low_latency")
		return 3;
	if (config.buffer_preset == "stable")
		return 8;
	if (config.buffer_preset == "custom")
		return plugin_config::ClampBufferChunks(config.buffer_chunks);
	if (config.buffer_preset != "auto") {
		obs_log(LOG_WARNING,
			"OutputInstance: unknown buffer_preset=\"%s\"; falling back to \"auto\" (4 chunks)",
			config.buffer_preset.c_str());
	}
	return 4;
}

std::vector<output_instance::MonitorOnlySource> OutputInstance::FindMonitorOnlySources(int audio_track)
{
	// REQ-UI-007/AC-24: 選択中トラック(mix_index)に割り当てられ、かつ monitoring type が
	// MONITOR_ONLY のソース名を集める。obs_enum_sources/obs_source_get_audio_mixers/
	// obs_source_get_monitoring_type はいずれも UI スレッドから呼んでよい公開 API
	// (ARCHITECTURE.md §12 事実 3)。
	struct EnumContext {
		uint32_t mix_bit;
		std::vector<obs_source_t *> *matched;
	};

	std::vector<obs_source_t *> matched;
	const size_t mix_index = audio_track_map::ToMixIndex(audio_track);
	EnumContext ctx{1u << (uint32_t)mix_index, &matched};

	obs_enum_sources(
		[](void *param, obs_source_t *source) -> bool {
			auto *context = static_cast<EnumContext *>(param);
			const uint32_t mixers = obs_source_get_audio_mixers(source);
			if ((mixers & context->mix_bit) == 0)
				return true; // このソースは選択中トラックに割り当てられていない。列挙継続。
			if (obs_source_get_monitoring_type(source) != OBS_MONITORING_TYPE_MONITOR_ONLY)
				return true;
			obs_source_t *ref = obs_source_get_ref(source);
			if (ref)
				context->matched->push_back(ref);
			return true;
		},
		&ctx);

	std::vector<output_instance::MonitorOnlySource> result;
	result.reserve(matched.size());
	for (obs_source_t *source : matched) {
		const char *name = obs_source_get_name(source);
		if (name && name[0] != '\0')
			result.push_back(output_instance::MonitorOnlySource{name});
		obs_source_release(source);
	}
	return result;
}
