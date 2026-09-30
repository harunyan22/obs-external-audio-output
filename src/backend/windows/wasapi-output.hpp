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

// 対応 REQ: REQ-AUDIO-002, REQ-AUDIO-004, REQ-AUDIO-005, REQ-DEVICE-001, REQ-NFR-002 / Slice 2, Slice 3,
// Slice 4
//
// WASAPI 共有モード・イベント駆動の出力バックエンド(wasapi-com.md 準拠)。
// IAudioClient/IAudioRenderClient の生成〜Release は専用の render thread 1 本に閉じる。
// Open() に渡された device_id("default" または IMMDevice::GetId() の文字列)で対象デバイスを
// 解決する(D-14、Slice 4)。id 解決自体(id 一致/名前一致)は device-matcher/OutputInstance の
// 責務で、WasapiOutput は解決済みの id をそのまま GetDevice()/GetDefaultAudioEndpoint() に渡すのみ。
// 1ch デバイスは Failed。非 float mix format は Slice 3 で AUTOCONVERTPCM により対応
// (ARCHITECTURE.md §5 手順 2.5、旧#18)。S2/S3/S4 では再接続しない(S6)。
// Slice 3: 読み出し経路(RenderLoop、pop 後・書き込み前)に DriftCompensator を組み込み、
// 水位ベースのサンプル挿入/間引きでドリフトを補正する(D-02 MVP)。

#pragma once

#include "../i-audio-output-backend.hpp"
#include "../../audio/drift-compensator.hpp"
#include "../../audio/ring-buffer.hpp"

#include <wrl/client.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class WasapiOutput final : public IAudioOutputBackend {
public:
	WasapiOutput() = default;
	~WasapiOutput() override;

	WasapiOutput(const WasapiOutput &) = delete;
	WasapiOutput &operator=(const WasapiOutput &) = delete;

	// device_id: "default"(または空文字列)なら GetDefaultAudioEndpoint(eConsole)、それ以外は
	// IMMDeviceEnumerator::GetDevice(device_id) で解決する(Slice 4、D-14)。id の解決(保存値との
	// 突き合わせ)自体は呼び出し元(OutputInstance + device-matcher)の責務で、ここでは受け取った
	// id をそのまま使う。失敗は種別ログを出して false を返す。
	bool Open(const std::string &device_id, uint32_t *out_sample_rate) override;
	void BindSource(RingBuffer *source) override;
	bool Start() override;
	void Stop() override;
	bool HasFailed() const override { return failed_.load(std::memory_order_acquire); }

	// Open() 成功後・Start() 前に 1 回だけ呼ぶ(BindSource() と同じ規約。control スレッド、RT では
	// ない)。target_level_frames は OutputInstance のプリロール目標を frames 換算した値
	// (kPrerollChunks * frames_per_chunk)と一致させること(ARCHITECTURE.md §7/§8、レビュー指摘
	// H-1)。frames_per_chunk は 1 チャンクの frames 数(デバイスレート換算)で、RenderLoop が
	// 毎 wake の frames 実効水位を計算するのにも使う。capacity_frames は RingBuffer の総容量を
	// frames 換算した値(発動閾値のクランプに使う)。device_sample_rate_hz は EMA の時定数換算に
	// 使う(旧: wake_interval_ms の近似値渡しをやめ、frames 進行量ベースに変更。レビュー指摘
	// Medium-1)。Slice 3 の実装内容 1/4/5。
	void ConfigureDriftCompensator(double target_level_frames, double frames_per_chunk, double capacity_frames,
				       double device_sample_rate_hz);

	// Open()/Start() が render thread の無応答タイムアウトで見捨てた(join せずに諦めた)状態か
	// (回帰レビュー指摘 #3)。true の間、Open()/Start() は何もせず即 false を返す。この
	// WasapiOutput インスタンスは復旧不能。
	//
	// Slice 6(S6-1、旧来の「detach して this を次の Open() で再利用する」設計を廃止): abandoned_ が
	// true になった WasapiOutput は、呼び出し元(OutputInstance の control thread)が二度と
	// Open()/Start() を呼ばない・二度と delete しない(ゾンビリストへ移して所有権ごと保持し続ける、
	// ARCHITECTURE.md §13 S6-1)。かわりに新しい WasapiOutput インスタンスで再接続を継続する。
	// これにより「見捨てた render thread が復帰して破棄済み/再利用済みの this を触る」UAF 経路が
	// 型レベルで無くなる(オブジェクト自体が生き続けるため、見捨てたスレッドが後から触れても
	// 未定義動作にはならない。リソースはプロセス終了まで解放されないリークを許容する設計判断)。
	bool IsAbandoned() const { return abandoned_.load(std::memory_order_acquire); }

	// Stop() が abandoned_ 状態の render thread を有限時間ポーリングしても join できず、
	// std::thread::detach() した場合に true になる(Stop() 呼び出し後にのみ意味を持つ)。true の場合、
	// 呼び出し元はこの WasapiOutput インスタンスを破棄してはならない(所有権ごとゾンビリストへ移し、
	// 新しい WasapiOutput で継続すること。S6-1)。false なら通常どおり破棄/再利用してよい。
	bool RequiresZombieRetention() const { return thread_detached_.load(std::memory_order_acquire); }

	// Running 状態遷移後に OutputInstance から呼ぶ(counting_enabled_ を立てる。RT ではない。
	// レビュー指摘 #10: WasapiOutput 自身の Start 成功ではなく OutputInstance の state_==Running
	// 遷移をトリガーにする)。
	void EnableUnderrunCounting() { counting_enabled_.store(true, std::memory_order_release); }

	// レビュー指摘(High-3): OutputInstance が構築(または再構築、S6-1 のゾンビ差し替え)のたびに
	// 自身の shutting_down_ を渡す。Open()/Start() の render thread ハンドシェイク待ちが、この
	// フラグが立っている間は通常の 5 秒(kControlWaitTimeout)ではなく短縮タイムアウトへ切り替わり、
	// EXIT の長時間ブロックを防ぐ。呼び出し元(control thread)が所有するオブジェクトへのポインタを
	// 保持するだけで、WasapiOutput 自身はこのポインタの生存管理をしない(OutputInstance が
	// WasapiOutput より長生きすることを前提とする。zombie_backends_ による意図的リークもこの前提を
	// 崩さない: OutputInstance 自体はプロセス終了まで生き続けるため)。
	void SetShutdownFlag(const std::atomic<bool> *shutting_down) { shutting_down_ = shutting_down; }

	// 開始時ダンプ(diagnostics)向けのフォーマット情報。Open() 成功後のみ有効。
	struct FormatInfo {
		uint32_t sample_rate = 0;
		uint16_t channels = 0;
		uint16_t bits_per_sample = 0;
		uint16_t format_tag = 0;
		bool is_float = false;
	};
	// IAudioClient::Initialize に実際に渡したストリーム形式(素の float、または AUTOCONVERTPCM の
	// ターゲット形式)。GetFormatInfo() という名前だが実体は「ストリーム形式」であることに注意
	// (再レビュー指摘 M: GetDeviceFormatInfo() と対で使う)。
	FormatInfo GetFormatInfo() const { return format_info_; }
	// GetMixFormat() が返したデバイスの生フォーマット(変換前の実値)。Initialize でどのフォーマットへ
	// 変換したか(GetFormatInfo())とは別物で、AUTOCONVERTPCM 使用時は両者が異なる(開始時ダンプで
	// device_format と stream_format を別々に出すため。再レビュー指摘 M)。Open() 成功後のみ有効。
	FormatInfo GetDeviceFormatInfo() const { return device_format_info_; }

	// WASAPI の実バッファサイズ(GetBufferSize() の値、frames)。Open() 成功後のみ有効
	// (OutputInstance のプリロール計算に使う。レビュー指摘 #3)。
	uint32_t GetBufferFrameCount() const { return buffer_frame_count_; }

	// Diagnostics の周期ログスレッドから読む統計(RT ではない読み取り)。
	uint64_t UnderrunCount() const { return underrun_count_.load(std::memory_order_relaxed); }
	// クリップしたサンプル数の累積(クリップが起きた wake の回数ではない。レビュー指摘 #13)。
	uint64_t ClipCount() const { return clip_count_.load(std::memory_order_relaxed); }

	// Level Meter(REQ-UI-005、D-15、Slice 5)。RenderLoop が「WASAPI 書き込み直前」に 1 wake
	// あたり 1 回だけ L/R の絶対値ピーク(0.0〜1.0 線形、クランプ後の値)を計算して store する
	// (rt-audio.md: per-wake ローカル集計→1 回の atomic store)。Dock の QTimer(33ms)が読む。
	float LevelPeakL() const { return level_peak_l_.load(std::memory_order_relaxed); }
	float LevelPeakR() const { return level_peak_r_.load(std::memory_order_relaxed); }

	// ドリフト補正の統計(診断スレッドから読む。RT ではない。drift_ 未構築なら 0/NaN を返す)。
	uint64_t DriftInsertedFrames() const { return drift_ ? drift_->InsertedFrames() : 0; }
	uint64_t DriftDroppedFrames() const { return drift_ ? drift_->DroppedFrames() : 0; }
	// Slice 6(S3 持ち越し、ブリーフ実装内容 13): 高速復帰モード中に適用された補正の累計 frames
	// (drift_ins/drop とは別カウント。「定常」と「復帰」を分離する)。
	uint64_t DriftRecoveryFrames() const { return drift_ ? drift_->RecoveryFrames() : 0; }
	// drift_ 未構築時は 0.0 ではなく NaN を返す(「未構築」と「構築済みで水位ちょうど 0」を
	// 診断ログで区別できるようにするため。レビュー指摘 Medium-2。診断側は std::isnan() で
	// "n/a" 表示に切り替える)。
	double DriftLevelEma() const
	{
		return drift_ ? drift_->EmaLevelFrames() : std::numeric_limits<double>::quiet_NaN();
	}

	// 非 float mix format のとき AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM を使って Initialize したか
	// (開始時ダンプ向け。Open() 成功後のみ有効。ブリーフ実装内容 6)。
	bool AutoConvertUsed() const { return autoconvert_used_; }

	// --- Slice 8(遅延・NFR 計測) ---

	// IAudioClient::GetStreamLatency() を Open()(render thread の OpenDeviceOnRenderThread())で
	// 1 回だけ取得した値(ms 換算)。GetStreamLatency 自体は RT ループの外(セットアップ専用フェーズ)で
	// 呼ぶ(ブリーフ実装内容 4)。取得に失敗した場合は 0.0(est_latency_ms からこの項は欠落する)。
	// 書き込みは render thread(セットアップ時、Open() の一度きり)、読み出しは診断スレッド/control
	// thread から任意タイミングで行うが、Open()→Start() のハンドシェイク(control_mutex_/control_cv_)と
	// diag_logger_.Start() が新規スレッドを起こす際の happens-before 関係により、値が確定した後にしか
	// 読まれない(format_info_ 等、既存の非 atomic セットアップ値と同じ規約)。
	double StreamLatencyMs() const { return (double)stream_latency_ref_time_ / 10000.0; }

	// ConfigureDriftCompensator() に渡された frames_per_chunk をそのまま返す(est_latency_ms の
	// 「リング実効 frames」計算に使う。diagnostics.cpp 参照)。Open() 成功後・Start() 前に確定し、
	// 以後セッション中は不変(StreamLatencyMs() と同じ happens-before 規約)。
	double FramesPerChunk() const { return drift_frames_per_chunk_; }

	// RenderLoop が毎 wake で GetCurrentPadding() から読んだ値をそのまま公開する(RT: atomic store
	// のみ。診断スレッドが est_latency_ms の「WASAPI バッファ滞留」項に使う)。
	uint32_t CurrentPaddingFrames() const { return current_padding_frames_.load(std::memory_order_relaxed); }

	// Slice 8 レビュー Medium-3: RenderLoop がこの wake の直前に計算した「リング実効 frames」
	// (source_->Level()*FramesPerChunk() + carry の残り frames)を、診断スレッドが est_latency_ms の
	// 「リング滞留」項にそのまま使えるよう公開する(RT: atomic store のみ)。以前の
	// ring_buffer->Level()*FramesPerChunk() による近似(診断スレッド側で再計算、carry 分を含まず
	// 最大 1 チャンク分の誤差があった)を置き換える。source_ 未設定/Open 前は 0。
	uint32_t CurrentLevelFrames() const { return current_level_frames_.load(std::memory_order_relaxed); }

	// render thread の自己 CPU 時間を計測する(AC-12)。rt-audio.md/ブリーフ実装内容 4:
	// GetThreadTimes は RT ループ(RenderLoop)の外で行う必要があるため、render thread 自身は呼ばない。
	// 代わりに DiagnosticsLogger の周期スレッド(2 秒ごと、RT ではない)が render thread の
	// native_handle() を対象に GetThreadTimes を呼び、前回サンプルとの差分/経過時間で 1 論理コア基準の
	// CPU% を計算して atomic へ格納する。render thread が joinable でなければ(未開始/既に
	// join/detach 済み)何もしない(直近の値を保持したまま)。
	void SampleCpuUsage();
	float RenderCpuPercent() const { return render_cpu_pct_.load(std::memory_order_relaxed); }
	// レビュー指摘 Low-9: stop summary が最後の値を出し終えた後、OutputInstance::CleanupFailedAttempt()
	// から呼ぶ(control thread、RT ではない)。Stopped 中に前セッションの値が残り続けないようにする
	// 衛生上の措置(Open() でも次セッション開始時に同様のリセットを行うが、Stop()〜次回 Open() の間の
	// 残留を無くす)。
	void ResetRenderCpuPercent() { render_cpu_pct_.store(0.0f, std::memory_order_relaxed); }

private:
	void ThreadMain();
	// render thread 内で実行する COM セットアップ(GetMixFormat〜GetService)。まだ定常 RT ループに
	// 入る前のセットアップ専用フェーズのため blog/例外的なログ出力を許容する。
	bool OpenDeviceOnRenderThread();
	// WaitForSingleObject〜ReleaseBuffer の定常ループ。ここに入ったら blog/ヒープ確保禁止(rt-audio.md)。
	void RenderLoop();
	// peak_l_accum/peak_r_accum: 呼び出し元(RenderLoop)がこの wake で保持しているローカル最大値
	// (絶対値、クランプ後)。この関数は現在の frames 分だけ見て、より大きければ更新する
	// (1 wake 内で複数回呼ばれても、最終的に RenderLoop が 1 回だけ atomic store する。D-15)。
	void WriteFrames(BYTE *dst, const float *stereo_src, uint32_t frames, float *peak_l_accum, float *peak_r_accum);
	void ZeroFillFrames(BYTE *dst, uint32_t frames);
	// COM リソース解放。render thread からのみ呼ぶ(wasapi-com.md)。
	void ReleaseDeviceResources();

	std::thread thread_;

	// Open()/Start() の同期用(一度きりのハンドシェイク。RT ではない)。述語に使う状態
	// (start_requested_/stop_requested_/open_done_/started_done_)の変更は必ずこの mutex を
	// 保持したまま行う(レビュー指摘 #1: notify_all() 自体はロック解放後でよい)。
	std::mutex control_mutex_;
	std::condition_variable control_cv_;
	bool open_done_ = false;
	// Start() は control_mutex_ を取らずにこの値を参照する(open 完了後は不変ではなく Open() の
	// 呼び出しごとに書き換わるため atomic にしてデータレースを避ける。レビュー指摘 #11)。
	std::atomic<bool> open_succeeded_{false};
	bool started_done_ = false;
	bool started_succeeded_ = false;

	std::atomic<bool> start_requested_{false};
	std::atomic<bool> stop_requested_{false};
	std::atomic<bool> failed_{false};
	std::atomic<bool> counting_enabled_{false}; // Running 遷移後のみ underrun を計上(rt-audio.md)

	// レビュー指摘(High-3): SetShutdownFlag() 参照。非所有(OutputInstance::shutting_down_ を指す)。
	const std::atomic<bool> *shutting_down_ = nullptr;

	// Open()/Start() の cv.wait_for がタイムアウトしたときに立てる(回帰レビュー指摘 #3)。
	// render thread が COM 呼び出し等でハングしている疑いがあり、join で待つと呼び出し元
	// (UI/frontend イベントスレッド)を無期限に止めかねないため、join せずに見捨てる。
	// 一度立ったら Open() では絶対にリセットしない(このインスタンスは以後ずっと使用不可。
	// Slice 6: 「新しい WasapiOutput で再接続を継続する」ことで復旧する。OBS 再起動は不要になった)。
	std::atomic<bool> abandoned_{false};

	// Stop() が abandoned_ な render thread を join できず detach した場合に true(Slice 6、S6-1)。
	// RequiresZombieRetention() で公開する。
	std::atomic<bool> thread_detached_{false};

	RingBuffer *source_ = nullptr;

	// Open() が control スレッドで書き込み、render thread(OpenDeviceOnRenderThread)が読む
	// (thread_ 生成前に書き込みが完了しているため std::thread の起動が happens-before を保証し、
	// atomic は不要。carry_len_ 等ほかの Open() 時セットアップと同じ規約)。"default" または空文字列
	// なら既定デバイス、それ以外は IMMDeviceEnumerator::GetDevice() に渡す id(Slice 4)。
	//
	// レビュー指摘 L-5(S6-1 の既知 UAF 対象拡大): Stop() が render thread を見捨てて detach する
	// 経路(abandoned_ == true、render thread が 5 秒超ハング)では、detach 後のスレッドが
	// OpenDeviceOnRenderThread() の途中から復帰すると device_id_ を含むこの WasapiOutput
	// インスタンス全体(this)へ触れ続け UAF になり得る。device_id_ 自身は Open() の再入で
	// 上書きされる単純な std::string だが、detach されたスレッドから見れば「所有者が別の
	// セッションに進んでいる間に書き換わる/破棄される可能性がある共有状態」の一つ。
	// ARCHITECTURE.md §13 S6-1 の再設計(見捨てたスレッドの安全な終端)でまとめて解消する。
	std::string device_id_;

	// render thread に閉じる COM リソース(wasapi-com.md: 生成〜Release は render thread 内で完結)。
	Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
	Microsoft::WRL::ComPtr<IMMDevice> device_;
	Microsoft::WRL::ComPtr<IAudioClient> audio_client_;
	Microsoft::WRL::ComPtr<IAudioRenderClient> render_client_;
	HANDLE render_event_ = nullptr;
	UINT32 buffer_frame_count_ = 0;

	FormatInfo format_info_{};
	// GetMixFormat() の生値(変換前)。OpenDeviceOnRenderThread() が GetMixFormat 成功直後に埋める
	// (再レビュー指摘 M)。
	FormatInfo device_format_info_{};

	std::atomic<uint64_t> underrun_count_{0};
	std::atomic<uint64_t> clip_count_{0};

	// Level Meter(D-15、Slice 5)。RenderLoop のみが store する(1 wake につき 1 回)。Dock の
	// QTimer のみが load する。初期値 0.0(無音)。
	std::atomic<float> level_peak_l_{0.0f};
	std::atomic<float> level_peak_r_{0.0f};

	// 失敗時の診断用(RT スレッドは atomic store のみ。ログ出力は RT ループを抜けた後に行う)。
	std::atomic<long> last_hr_{0};
	std::atomic<unsigned long> last_win32_err_{0};

	// RingBuffer から pop したチャンクのうち device バッファへ書ききれなかった残り。
	// BindSource() で source->MaxChunkFrames() に合わせて 1 回だけ確保する(RT ではない。
	// レビュー指摘 #2: RingBuffer のチャンク上限が構築時引数になったことに追従)。
	// render thread のみが読み書きする(確保後は再確保しない)。
	std::unique_ptr<float[]> carry_buf_;
	// carry_buf_ が保持できる frames 数(RenderLoop の #ifndef NDEBUG チェックで使う。レビュー指摘 L-7)。
	uint32_t carry_capacity_ = 0;
	uint32_t carry_len_ = 0;
	uint32_t carry_pos_ = 0;

	// ドリフト補正(Slice 3、実装内容 1〜7)。ConfigureDriftCompensator() で Open() 成功後・Start()
	// 前に construct する(control スレッド)。render thread(RenderLoop)のみが OnWake()/
	// NotifyApplied() を呼ぶ。
	std::unique_ptr<DriftCompensator> drift_;
	// ConfigureDriftCompensator() で渡された frames_per_chunk(RenderLoop が毎 wake の frames
	// 実効水位 = source_->Level()*drift_frames_per_chunk_ + (carry_len_-carry_pos_) を計算するのに
	// 使う。レビュー指摘 H-1)。
	double drift_frames_per_chunk_ = 0.0;
	// 挿入(直前 frame の複製)用に保持する、直近に device_buffer へ書き込んだ 1 frame(L, R)。
	// render thread のみが読み書きする(RT: ヒープ確保なし、単純な配列)。
	float last_frame_[2] = {0.0f, 0.0f};
	bool has_last_frame_ = false;

	// 非 float mix format に対して AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM を使って Initialize したか
	// (開始時ダンプ向け)。
	bool autoconvert_used_ = false;

	// --- Slice 8(遅延・NFR 計測) ---

	// IAudioClient::GetStreamLatency() の生値(100ns 単位)。OpenDeviceOnRenderThread() が Initialize
	// 成功後に 1 回だけ書き込む(RT ではないセットアップフェーズ)。以後は不変(StreamLatencyMs() の
	// コメント参照)。
	REFERENCE_TIME stream_latency_ref_time_ = 0;

	// RenderLoop の毎 wake で GetCurrentPadding() の値を store するだけ(RT 安全)。
	std::atomic<uint32_t> current_padding_frames_{0};

	// Slice 8 レビュー Medium-3: CurrentLevelFrames() 参照。RenderLoop の毎 wake で
	// source_->Level()*drift_frames_per_chunk_ + carry の残り frames を計算し、store するだけ
	// (RT 安全、ヒープ確保・ロックなし)。
	std::atomic<uint32_t> current_level_frames_{0};

	// SampleCpuUsage() 専用の状態(DiagnosticsLogger の周期スレッドのみが読み書きする単一書き手/
	// 単一呼び出し元のため atomic 不要。render_cpu_pct_ だけが他スレッドから読まれるため atomic)。
	uint64_t cpu_sample_prev_cpu_100ns_ = 0;
	uint64_t cpu_sample_prev_wall_ns_ = 0;
	bool cpu_sample_has_prev_ = false;
	std::atomic<float> render_cpu_pct_{0.0f};
};
