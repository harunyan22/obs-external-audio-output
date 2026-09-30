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

// 対応 REQ: REQ-AUDIO-002, REQ-AUDIO-003, REQ-AUDIO-004, REQ-AUDIO-007, REQ-DEVICE-001,
// REQ-DEVICE-002, REQ-DEVICE-003, REQ-DEVICE-004, REQ-DEVICE-005, REQ-LIFECYCLE-001, REQ-LOG-001,
// REQ-STABILITY-001, REQ-UI-001, REQ-UI-005, REQ-UI-006, REQ-UI-007, REQ-UI-008 / Slice 2, Slice 4,
// Slice 5, Slice 6
//
// AudioMixSource → RingBuffer → WasapiOutput の配線と、5 状態機械(Stopped/Starting/Running/
// Recovering/Failed、REQUIREMENTS.md §4)を担う。
//
// Slice 6(D-10 ③、本ファイルの中核変更): OutputInstance が専用の control thread を 1 本所有する。
// デバイス列挙・Open/Close・再接続・状態遷移は「すべて」この control thread 上で直列実行し、UI
// (Qt)/frontend イベントスレッドからは Start()/Stop()/Restart()/RequestReregister() が
// ControlQueue へコマンドを積んで即座に return するだけになる(S5 High-2「UI 同期ブロックの構造対応」の
// 解消)。GetState() は control thread が書き込む state_ の単純な atomic load(副作用なし。
// S2〜S5 にあった「GetState() が Failed への遅延遷移を行う」設計は本 Slice で廃止した)。
//
// UI スレッドから読む値(LevelPeakL/R、BuildDiagnosticsSnapshot、GetEffectiveConfig)は ui_mutex_ で
// 保護する。この mutex は「control thread が保持オブジェクト(backend_/ring_buffer_)を差し替える
// 一瞬」だけ短時間保持するものであり、COM 呼び出しや render thread の join のような重い/長時間処理は
// 必ずロックの外で行ってから差し替える(UI スレッドを長時間ブロックしないための設計。詳細は .cpp の
// TeardownActiveSession()/ReplaceBackendIfZombified() のコメント)。

#pragma once

#include "../audio/audio-mix-source.hpp"
#include "../audio/ring-buffer.hpp"
#include "../audio/track-map.hpp"
#include "../backend/windows/wasapi-output.hpp"
#include "../device/device-notifier.hpp"
#include "../diag/diagnostics.hpp"
#include "control-queue.hpp"
#include "reconnect-policy.hpp"

#include <wrl/client.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace output_instance {

enum class State { Stopped, Starting, Running, Recovering, Failed };

// LoadConfig() が返す、config.json 由来(または既定値)の設定一式(REQUIREMENTS.md §3)。
struct ConfigValues {
	int audio_track = audio_track_map::kDefaultAudioTrack;
	std::string device_id = "default";
	std::string device_name;
	// "auto" / "low_latency" / "stable" / "custom"(REQUIREMENTS.md §3)。
	std::string buffer_preset = "auto";
	// buffer_preset=="custom" のときのみ有効(2〜16、REQUIREMENTS.md §3)。
	uint32_t buffer_chunks = 4;
	// Slice 6 で動作に反映される(切断検知時の Recovering 遷移可否、ブリーフ実装内容 5)。
	bool auto_reconnect = true;
	// "error" / "warning" / "info" / "debug"。
	std::string log_level = "info";
};

// Restart() に渡す変更差分。std::nullopt のフィールドは現在の実効値を変更しない。指定した
// フィールドは OutputInstance 内部のランタイムオーバーライドへ蓄積され、以後の Start()/Restart()
// にも引き継がれる。Slice 7: HandleRestartCommand() が実効値へ反映すると同時に config.json へも
// 保存する(実装内容 実装内容の要点 1c)。
struct ConfigOverrides {
	std::optional<int> audio_track;
	std::optional<std::string> device_id;
	std::optional<std::string> device_name;
	std::optional<std::string> buffer_preset;
	std::optional<uint32_t> buffer_chunks;
	std::optional<bool> auto_reconnect;
	std::optional<std::string> log_level;
};

// MONITOR_ONLY 警告対象の 1 件(REQ-UI-007)。
struct MonitorOnlySource {
	std::string name;
};

// control thread が処理するコマンド(UI/frontend イベントスレッドが enqueue する。Slice 6)。
enum class ControlCommandType {
	Start,
	Stop,
	Restart,
	// PROFILE_CHANGED(obs_reset_audio 追従、D-06、ブリーフ実装内容 10)。
	Reregister,
	// EXIT シーケンスでのみ enqueue する。control thread はこれを処理した後ループを抜ける。
	Shutdown,
};

struct ControlCommand {
	ControlCommandType type = ControlCommandType::Start;
	ConfigOverrides overrides; // Restart のみ使用。
};

} // namespace output_instance

class OutputInstance {
public:
	OutputInstance();
	~OutputInstance();

	OutputInstance(const OutputInstance &) = delete;
	OutputInstance &operator=(const OutputInstance &) = delete;

	// OBS_FRONTEND_EVENT_FINISHED_LOADING で呼ぶ(plugin-main.cpp、ブリーフ実装内容 1)。control
	// thread を起動する。二重呼び出しは no-op。この時点では音声処理を自動開始しない(control thread の
	// インフラを起動するだけ。実際の Start はコマンド enqueue 経由。qt-ui-and-lifecycle.md の
	// 「obs_module_load では音声処理を開始しない」を FINISHED_LOADING まで単純に引き延ばした形)。
	void StartControlThread();

	// OBS_FRONTEND_EVENT_EXIT で呼ぶ。Stop → Shutdown コマンドを enqueue し、control thread の終了を
	// 待つ(ブロックする)。EXIT シーケンス自体は元々同期的だったため許容する(qt-ui-and-lifecycle.md/
	// ブリーフ: Dock タイマー停止 → control thread へ Stop 要求 → control thread join)。
	// StartControlThread() を呼んでいない場合、または二重呼び出しは no-op。
	void Shutdown();

	// UI/frontend イベントスレッドから呼ぶ。コマンドを ControlQueue へ積んで即座に return する
	// (構造的非同期化、S6 の中核)。実際の遷移は control thread が非同期に行う。呼び出し元は
	// GetState() のポーリング(Dock は 500ms)で結果を観測すること。
	// レビュー指摘(Medium-4): StartControlThread() 呼び出し前、または Shutdown() 開始後は
	// コマンドを受け付けない(CanAcceptCommands() が false を返し、無言で破棄せず LOG_WARNING を
	// 1 回出したうえで push しない)。
	void Start();
	void Stop();

	// backend_.HasFailed() の検査・状態遷移はすべて control thread が行うため、GetState() は単純な
	// atomic load(副作用なし)。
	output_instance::State GetState() const { return state_.load(std::memory_order_acquire); }
	bool IsRunning() const { return GetState() == output_instance::State::Running; }

	// レビュー指摘(High-2b): MonitorCompatController が自己無音警告のデバイス列挙キャッシュを
	// 無効化するかどうかの判定に使う。control thread の HandleDeviceEvent() が
	// DefaultDeviceChanged/DeviceArrived を受信するたびに増分する(atomic load のみ、副作用なし)。
	uint64_t DeviceTopologyChangeCount() const
	{
		return device_topology_change_count_.load(std::memory_order_acquire);
	}

	// Dock の詳細設定(Advanced)から呼ぶ。Restart コマンドを enqueue して即座に return する。
	// overrides で指定したフィールドだけをランタイムオーバーライドへ反映し、同時に config.json へも
	// 保存する(Slice 7、enabled は変更しない)。log_level は音声パスに影響しないため control thread
	// が即座に反映する。audio_track/device_id/
	// device_name/buffer_preset/buffer_chunks はいずれか 1 つでも変更があり、かつ現在
	// Starting/Running/Recovering なら control thread が Stop→Start 相当を直列に行って新設定を
	// 反映する。Stopped/Failed 中は次回 Start() で自動的に反映されるだけで、ここでは出力を起動しない。
	void Restart(const output_instance::ConfigOverrides &overrides);

	// OBS_FRONTEND_EVENT_PROFILE_CHANGED(obs_reset_audio 追従、D-06)。plugin-main.cpp のフロント
	// エンドイベントハンドラから呼ぶ。Reregister コマンドを enqueue して即座に return する。
	void RequestReregister();

	// config.json の実効値(ランタイムオーバーライド適用後)を返す(副作用なし)。Dock の詳細設定
	// パネルの初期表示に使う。UI スレッドから呼ぶ。
	output_instance::ConfigValues GetEffectiveConfig() const;

	// Level Meter(REQ-UI-005、D-15)。WasapiOutput の per-wake atomic をそのまま橋渡しする
	// (RT ではない)。ui_mutex_ 配下で現在の backend ポインタを読み、保持したまま呼ぶ(control thread
	// が対象を破棄する前に必ず ui_mutex_ 配下でポインタを null 化するため UAF にならない。
	// output-instance.cpp のコメント参照)。
	float LevelPeakL() const;
	float LevelPeakR() const;

	// AC-22 の 15 項目スナップショットを取得する(REQ-LOG-001, REQ-UI-008)。UI スレッド・control
	// thread のどちらから呼んでも安全(ui_mutex_ で保護)。Slice 9: output_mode と互換モードの
	// イベントカウンタ(SetCompatEventCounts() 参照)も併せて含む(AC-22 の 15 項目とは別枠の追加情報、
	// 既存項目の意味は変えない)。
	diagnostics::DiagnosticsSnapshot BuildDiagnosticsSnapshot() const;

	// Slice 9(REQ-COMPAT-001): output_mode の読み書き。config.json の他フィールドと同じ
	// read-modify-write 直列化(config_persist_mutex_)を経由する。control thread の
	// HandleStartCommand() が排他ガードとして GetPersistedOutputMode() を読む。UI スレッド
	// (ExternalOutputDock の OutputMode コンボ)が SetPersistedOutputMode() で書く。
	std::string GetPersistedOutputMode() const;
	bool SetPersistedOutputMode(const std::string &output_mode);

	// レビュー指摘(High-1): 互換モードの有効化/無効化は、独立出力と同じ「ユーザー意思のみ」規約で
	// enabled を永続化する(compat_controller_.Enable()/Disable() の成否に応じて ExternalOutputDock
	// が呼ぶ)。config.json の他フィールドと同じ read-modify-write 直列化(config_persist_mutex_)を
	// 経由する。
	bool SetPersistedEnabled(bool enabled);

	// Slice 9: MonitorCompatController のイベントカウンタ(適用/復元/再適用回数)を診断スナップショットへ
	// 反映するための橋渡し。ExternalOutputDock が Enable()/Disable()/Reapply() のたびに呼ぶ
	// (UI スレッドから、ui_mutex_ で保護)。
	void SetCompatEventCounts(uint64_t apply_count, uint64_t restore_count, uint64_t reapply_count);

	// 選択中トラック(audio_track、1〜6)に割り当てられたソースのうち monitoring type が
	// MONITOR_ONLY のものを列挙する(REQ-UI-007、AC-24)。OutputInstance の状態を読まない静的関数。
	static std::vector<output_instance::MonitorOnlySource> FindMonitorOnlySources(int audio_track);

private:
	// --- control thread 本体(ブリーフ実装内容 1〜14) ---
	void ControlThreadMain();
	void HandleCommand(const output_instance::ControlCommand &cmd);
	void HandleStartCommand();
	void HandleRestartCommand(const output_instance::ConfigOverrides &overrides);
	void HandleReregisterCommand();
	void HandleDeviceEvent(const device_notifier::Event &event);
	// device_id の flow(eRender/eCapture)を notify_enumerator_ 経由で判定する(レビュー指摘
	// Medium-5)。control thread 上の通常の COM 呼び出し(IMMNotificationClient コールバック自身の
	// 中ではない)なので旧#22 の禁止 4 項目の対象外。判定できなければ std::nullopt を返す
	// (デバイス切断直後の競合等で GetDevice/QueryInterface が失敗するケース)。
	std::optional<bool> IsRenderDeviceId(const std::string &device_id) const;
	// UI/frontend イベントスレッドから呼ばれる Start()/Stop()/Restart()/RequestReregister() の
	// 入口ガード(レビュー指摘 Medium-4)。StartControlThread() 未呼び出し、または Shutdown() 開始後
	// (shutting_down_==true)ならコマンドを積まずに false を返す(呼び出し元は無言で諦めず
	// LOG_WARNING を 1 回出す)。
	bool CanAcceptCommands() const;
	// backend_->HasFailed() 検出(Running 中の render thread 失敗)を受けて Recovering/Failed へ遷移する
	// (ブリーフ実装内容 3〜5)。auto_reconnect==true なら Recovering、false なら直接 Failed。
	void HandleRuntimeFailureDetected();
	void RunPeriodicTasks();
	void RunWatchdog(uint64_t now_ns);
	void RunReconnectTick(uint64_t now_ns);
	void RunDebouncedDefaultDeviceChange(uint64_t now_ns);
	// Slice 8(AC-12): control thread 自身の GetThreadTimes を 2 秒ごとにサンプルし cpu_control_pct_ へ
	// 反映する。control thread は RT ループではないため GetCurrentThread() を自スレッドから直接呼んで
	// よい(rt-audio.md の対象は RenderLoop/callback のみ)。RunPeriodicTasks() から state_ に関わらず
	// 毎ティック呼び、内部で 2 秒間隔をガードする(Stopped 中もアイドル CPU を追える)。
	void SampleControlThreadCpu(uint64_t now_ns);
	// ピンポイント修正(AC-12 アイドル CPU 測定): Stopped 状態のときのみ 60 秒ごとに 1 行
	// "idle stats: cpu_control_cum_pct=X.XXX" を LOG_INFO で出す(Running/Recovering 中は出さない)。
	// レビュー指摘 Medium-2: cpu_control_pct_(2 秒窓)ではなく GetThreadTimes() creation 以降の
	// 累積 CPU%(窓長非依存)を出す。RunPeriodicTasks() から毎ティック呼び、内部で状態判定と 60 秒
	// 間隔をガードする(ログスパム防止)。
	void LogIdleStatsIfDue(uint64_t now_ns);
	// AudioMixSource::Reregister() を device_sample_rate_for_reregister_ で呼ぶ共通ヘルパ
	// (PROFILE_CHANGED ハンドラ・ウォッチドッグの両方から使う)。
	void ReregisterMixSourceNow();

	// Starting/Recovering 共通の接続試行本体(旧 Start() の中身)。config は呼び出し時点の実効値
	// (LoadConfig()+overrides_、device_id/device_name は呼び出し元が reconnect_device_id_/name_ 等で
	// 上書き済みのものを渡す)。成功なら state_ を Running へ遷移して true を返す。失敗時は state_
	// を変更せず false を返す(Recovering/Failed への遷移は呼び出し元が決める)。内部で必ず
	// CleanupFailedAttempt() を先頭で呼ぶ(前回の部分状態の一掃、初回 Starting でも安全な no-op)。
	bool AttemptConnect(const output_instance::ConfigValues &config);

	// mix_source_/ring_buffer_/backend_ を後始末する(diag_logger_ を先に止めてから)。ever_running_
	// (このセグメントで一度でも Running に到達していたか)が true なら ring_buffer_ を破棄する前に
	// DiagnosticsLogger::LogFinalSummary() で最終値のサマリを出してから ever_running_ を false に戻す
	// (Recovering への遷移も「1 セグメントの終わり」として扱う。再接続成功時に AttemptConnect() が
	// 新しいセグメントとして ever_running_ を再び true にするため、切断のたびに 1 行、要約ログが出る)。
	// Start コマンド・Restart・Recovering の再試行・実行時失敗検出・Stop・EXIT のすべてから呼ばれる
	// 共通の後始末。
	void CleanupFailedAttempt();

	// Stop コマンド・EXIT で呼ぶ完全停止(旧 Stop() 相当)。CleanupFailedAttempt() に加えて AC-22 の
	// 診断スナップショットを出す(開始ダンプ・停止サマリのいずれかに 15 項目すべてが出ることを保証)。
	void TeardownActiveSession();

	// WasapiOutput::RequiresZombieRetention()==true のとき、backend_ をゾンビリストへ移し(所有権
	// ごと保持し続け、プロセス終了まで解放しない)、新しい WasapiOutput を構築して差し替える(S6-1)。
	void ReplaceBackendIfZombified();

	void TransitionTo(output_instance::State new_state);
	void SetInitError(const std::string &message);
	void IncrementReconnectEvents();

	// Slice 7(REQ-CONFIG-001/002, D-13/D-14): config.json へ実効値を書き戻す。enabled が
	// std::nullopt ならディスク上の既存 enabled 値を変更しない(Restart() 経由の設定変更では
	// enabled に触れない)。呼び出しは HandleStartCommand() の Start 成功時(enabled=true)、Stop()
	// の明示的ユーザー停止時(enabled=false)、HandleRestartCommand() の Advanced 設定変更時
	// (enabled=std::nullopt)の 3 箇所のみ。control thread・UI/frontend イベントスレッドのどちらから
	// 呼んでも安全(ファイル I/O のみ、RT では呼ばない)。
	void PersistEffectiveConfig(std::optional<bool> enabled, const output_instance::ConfigValues &config);

	// buffer_preset/buffer_chunks から実際のチャンク数を解決する(REQUIREMENTS.md §3: low_latency=3 /
	// auto=4 / stable=8、custom は buffer_chunks を 2〜16 にクランプ)。
	static uint32_t ResolveBufferChunks(const output_instance::ConfigValues &config);

	// --- スレッド/キュー(control thread と UI/frontend イベントスレッド・IMMNotificationClient
	// コールバックの橋渡し) ---
	std::thread control_thread_;
	std::atomic<bool> control_thread_started_{false};
	ControlQueue<output_instance::ControlCommand> command_queue_;
	ControlQueue<device_notifier::Event> device_events_;
	// レビュー指摘(High-3): Shutdown() の冒頭で立てる。control thread が AttemptConnect()(デバイス
	// 列挙・WasapiOutput::Open/Start)の途中にいても各フェーズ境界で即座に中断できるようにし、EXIT の
	// 長時間ブロックを防ぐ。WasapiOutput にもポインタで共有し、Open()/Start() の render thread
	// ハンドシェイク待ちを 5s→500ms へ短縮させる(backend_ 構築のたびに SetShutdownFlag() で渡す)。
	// 一度立てたら戻さない(Shutdown() は 1 回きりの想定、qt-ui-and-lifecycle.md の EXIT シーケンス)。
	std::atomic<bool> shutting_down_{false};

	// UI からポーリングされる状態。control thread のみが store する。
	std::atomic<output_instance::State> state_{output_instance::State::Stopped};

	// レビュー指摘(High-2b): MonitorCompatController の自己無音警告キャッシュ無効化のためのトポロジ
	// 変更カウンタ。control thread の HandleDeviceEvent() が DefaultDeviceChanged/DeviceArrived を
	// 受信するたびに増分する(過剰無効化は安全側なのでイベント種別は問わない)。DeviceTopologyChangeCount()
	// が atomic load するだけの副作用なし。
	std::atomic<uint64_t> device_topology_change_count_{0};

	// --- control thread 専有(単一スレッドのみが読み書きするためロック不要) ---
	std::unique_ptr<RingBuffer> ring_buffer_;
	AudioMixSource mix_source_;
	std::unique_ptr<WasapiOutput> backend_;
	DiagnosticsLogger diag_logger_;
	bool ever_running_ = false;
	// 直近の AttemptConnect() 成功時に解決したデバイスのミックスレート。ReregisterMixSourceNow() が
	// AudioMixSource::Reregister() に渡す(obs_reset_audio は OBS 内部レートを変えるだけで WASAPI
	// デバイス側は開いたまま変わらないため、Start() 時点の値をそのまま使い続けてよい。ブリーフ実装
	// 内容 10)。
	uint32_t device_sample_rate_for_reregister_ = 0;
	// レビュー指摘(High-1): 直近の AttemptConnect() 成功時点の obs_get_audio_info().samples_per_sec。
	// HandleReregisterCommand()/RunWatchdog() がこの値と現在の obs_get_audio_info() を比較し、
	// 変化していれば(obs_reset_audio でリサンプル比が変わった)単純な Reregister() では
	// RingBuffer::max_chunk_frames_ 等の旧値のサイジングが追従できないため、TeardownActiveSession()
	// → HandleStartCommand() 相当の完全再構築へ切り替える。
	uint32_t obs_sample_rate_at_start_ = 0;

	ReconnectPolicy reconnect_policy_;
	// Recovering 再接続で使う「保存済み device_id/device_name」。Recovering に入った時点の実効設定
	// から取得し、以後の再試行のたびに config.json を読み直さない(ブリーフ実装内容 4)。
	std::string reconnect_device_id_ = "default";
	std::string reconnect_device_name_;
	uint64_t next_reconnect_attempt_ns_ = 0; // 0 は「即座に試行してよい」。
	// レビュー指摘(Medium-5): DeviceArrived の flow/id を判定できなかった(GetDevice/QueryInterface
	// 失敗等)場合のフォールバックとして、Recovering 中のバックオフリセットを許容する回数の上限。
	// State::Recovering へ遷移するたびに TransitionTo() が 0 へリセットする。
	static constexpr int kMaxArrivalResetFallback = 3;
	int arrival_reset_fallback_count_ = 0;

	// ウォッチドッグ(ブリーフ実装内容 11)。Running 時のみ 500ms 周期でチェックする。
	uint64_t watchdog_last_callback_count_ = 0;
	uint64_t watchdog_last_progress_ns_ = 0;
	bool watchdog_warned_ = false;
	uint64_t watchdog_last_check_ns_ = 0;

	// Slice 8(AC-12): control thread の自己 CPU 時間サンプリング状態(control thread のみが
	// 読み書きするため atomic 不要。cpu_control_pct_ だけが diag_logger_ の周期スレッドから読まれるため
	// atomic)。SampleControlThreadCpu() 参照。
	uint64_t cpu_sample_prev_cpu_100ns_ = 0;
	uint64_t cpu_sample_prev_wall_ns_ = 0;
	uint64_t cpu_sample_last_check_ns_ = 0;
	bool cpu_sample_has_prev_ = false;
	std::atomic<float> cpu_control_pct_{0.0f};

	// ピンポイント修正(AC-12): アイドル(Stopped)状態での周期ログの直近出力時刻。0 は「まだ Stopped の
	// 計測ウィンドウを開始していない」を意味し、Stopped に入った/戻った直後はこの値を 0 にリセットして
	// 60 秒待ってから最初の 1 行を出す(LogIdleStatsIfDue() 参照)。
	uint64_t idle_stats_last_log_ns_ = 0;

	// OnDefaultDeviceChanged のデバウンス(ブリーフ実装内容 7、200ms)。
	bool default_device_change_pending_ = false;
	uint64_t default_device_change_fire_ns_ = 0;

	// IMMNotificationClient(control thread 専有。Register/Unregister もここでのみ操作する。
	// ARCHITECTURE.md §12 旧#22)。
	Microsoft::WRL::ComPtr<IMMDeviceEnumerator> notify_enumerator_;
	// レビュー指摘(High-2): EXIT で Unregister() した後、Release() は呼ばず意図的にリークする
	// (delete しない。zombie_backends_ と同じ方針)。理由: Unregister() が「呼び出し中の
	// コールバックの完了を待つ」保証を公式ドキュメント/OBS 実装から確認できておらず、Release()
	// で delete したタイミングと in-flight のコールバックが競合すると UAF になり得るため。
	// DeviceNotifier::Disable() を Unregister() の前に呼び、in-flight/以後のコールバックはすべて
	// 早期 return させたうえで、オブジェクト自体はプロセス終了まで生かし続ける。
	DeviceNotifier *device_notifier_ = nullptr; // 参照カウント自前管理(旧#22: Register は AddRef しない)。

	// S6-1: ゾンビ化した WasapiOutput を意図的にリークして保持する(delete しない。EXIT でも解放
	// しない。コメントは wasapi-output.hpp/.cpp、ReplaceBackendIfZombified() 参照)。
	std::vector<WasapiOutput *> zombie_backends_;
	// ゾンビ累積上限(ピンポイント修正: 3 個到達で以後の自動 Recovering を止める)。3 個目の
	// ReplaceBackendIfZombified() でこの上限に達すると true になり、二度と false へ戻らない
	// (プロセス寿命の間、以後の自動再接続と手動 Start()/「再接続」ボタンをすべて Failed 固定にする)。
	static constexpr size_t kMaxZombieBackends = 3;
	bool zombie_limit_reached_ = false;

	// --- UI スレッドから読む値(ui_mutex_ で保護。常に短時間だけ保持する。ヘッダ冒頭コメント参照) ---
	mutable std::mutex ui_mutex_;
	output_instance::ConfigOverrides overrides_;
	WasapiOutput *backend_ptr_ = nullptr;   // LevelPeakL/R・BuildDiagnosticsSnapshot 用。
	RingBuffer *ring_buffer_ptr_ = nullptr; // BuildDiagnosticsSnapshot 用。
	std::string selected_device_id_;
	std::string selected_device_name_;
	uint32_t last_obs_sample_rate_ = 0;
	uint32_t last_output_sample_rate_ = 0;
	uint16_t last_channel_count_ = 0;
	uint32_t last_buffer_chunks_ = 0;
	uint32_t last_buffer_chunks_requested_ = 0;
	std::string last_init_error_ = "none";
	uint64_t reconnect_events_ = 0; // Recovering→Running 成功のたびに増える(ブリーフ実装内容 4)。

	// Slice 9: SetCompatEventCounts() が書き込む MonitorCompatController のイベントカウンタ
	// (BuildDiagnosticsSnapshot() が読む)。
	uint64_t compat_apply_count_ = 0;
	uint64_t compat_restore_count_ = 0;
	uint64_t compat_reapply_count_ = 0;

	// Slice 7: PersistEffectiveConfig() は control thread(HandleStartCommand()/
	// HandleRestartCommand())と UI/Qt Tools メニューのイベントスレッド(Stop())の両方から
	// 呼ばれ得る。plugin_config::Load() → 該当フィールドだけ上書き → plugin_config::Save() という
	// read-modify-write を 2 スレッドが同時に行うと、後勝ちの Save() がもう一方の変更を静かに
	// 消してしまう(lost update)。config.json への書き込みは低頻度・RT ではないため、
	// PersistEffectiveConfig() 呼び出し全体をこの mutex で直列化し、少なくとも「実際に先に
	// PersistEffectiveConfig() を完了した側の内容を後続の Load() が必ず見る」順序を保証する
	// (2 つの呼び出しの意味的な優先順位(例: ほぼ同時の Restart と Stop のどちらを「最終結果」と
	// するか)まではこれだけでは解決しない。呼び出し元の同時発生自体は珍しくない想定のため、
	// 報告時の懸念事項として明記する)。
	// Slice 9: GetPersistedOutputMode()(const メソッド)からも同じ直列化で読むため mutable にする。
	mutable std::mutex config_persist_mutex_;

	// レビュー指摘(Medium-4): output_mode のメモリキャッシュ。BuildDiagnosticsSnapshot() が
	// GetPersistedOutputMode() を毎回呼ぶたびに config.json を読み直すのを避ける。SetPersistedOutputMode()
	// が書き込みと同時に更新する。GetPersistedOutputMode() は初回呼び出し時のみ plugin_config::Load()
	// で初期化する(以後は SetPersistedOutputMode() のみが更新する唯一の書き手)。config_persist_mutex_
	// で保護するため mutable にする。
	mutable std::string output_mode_cache_ = "independent";
	mutable bool output_mode_cache_initialized_ = false;
};
