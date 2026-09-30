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

#include "wasapi-output.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <avrt.h>
#include <mmreg.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <system_error>

using Microsoft::WRL::ComPtr;

namespace {

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT と同一の GUID(ksmedia.h の vendoring/INITGUID 回避のため
// ローカルに定義する。DEFINE_GUID を経由しないため INITGUID の要不要を気にしなくてよい)。
constexpr GUID kSubTypeIeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// Open()/Start() の cv wait に付けるタイムアウト(レビュー指摘 #1)。render thread が応答しない
// 異常系(COM 呼び出しのハング等)でメインスレッドを永久に眠らせないための保険。
constexpr auto kControlWaitTimeout = std::chrono::seconds(5);

// レビュー指摘(High-3): EXIT 中(OutputInstance::shutting_down_==true)は上記 5 秒を待たず、この
// 短縮タイムアウトで諦める(abandoned_ 扱いにして control thread を先へ進める。EXIT の長時間
// ブロック対策。目標: 通常 <1s、最悪でも ~3s)。
constexpr auto kShutdownWaitTimeout = std::chrono::milliseconds(500);
// shutting_down_ の変化を取りこぼさないためのポーリング粒度(control thread から
// OutputInstance::Shutdown() が shutting_down_ を立てるのは、この cv 待ちの最中であり得るため)。
constexpr auto kShutdownPollInterval = std::chrono::milliseconds(100);

// cv.wait_for(lock, timeout, pred) 相当だが、shutting_down が非 null かつ立っている間は
// kShutdownWaitTimeout、そうでなければ kControlWaitTimeout を締切として使う。shutting_down は
// 待機中に(別スレッドの OutputInstance::Shutdown() によって)立つことがあるため、
// kShutdownPollInterval 刻みでポーリングし、都度どちらの締切を使うか再評価する。
template<typename Pred>
bool WaitForRenderThreadHandshake(std::condition_variable &cv, std::unique_lock<std::mutex> &lock,
				  const std::atomic<bool> *shutting_down, Pred pred)
{
	const auto wait_start = std::chrono::steady_clock::now();
	for (;;) {
		const bool is_shutting_down = shutting_down && shutting_down->load(std::memory_order_acquire);
		const auto total_timeout = is_shutting_down ? kShutdownWaitTimeout : kControlWaitTimeout;
		const auto deadline = wait_start + total_timeout;
		const auto now = std::chrono::steady_clock::now();
		if (now >= deadline)
			return pred(); // cv.wait_for の流儀に合わせ、締切到達時点の述語値を返す。
		const auto slice = std::min<std::chrono::steady_clock::duration>(kShutdownPollInterval, deadline - now);
		if (cv.wait_for(lock, slice, pred))
			return true;
	}
}

// abandoned_ 状態からの Stop() で、見捨てた render thread が実は既に終了していないかを
// native_handle() 越しにポーリングする合計時間(回帰レビュー指摘 #3)。この時間を過ぎても
// 終了が確認できなければ join せず detach する。
constexpr auto kAbandonedJoinWaitTimeout = std::chrono::seconds(2);

bool IsFloatFormat(const WAVEFORMATEX *fmt)
{
	if (!fmt)
		return false;
	if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
		return true;
	if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
		const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(fmt);
		return IsEqualGUID(ext->SubFormat, kSubTypeIeeeFloat);
	}
	return false;
}

// HRESULT を診断ログ向けに分類する(レビュー指摘 #5)。回復処理自体は S6 で実装する。
const char *ClassifyHr(HRESULT hr)
{
	switch (hr) {
	case AUDCLNT_E_DEVICE_INVALIDATED:
		return "DEVICE_INVALIDATED";
	case AUDCLNT_E_RESOURCES_INVALIDATED:
		return "RESOURCES_INVALIDATED";
	case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED:
		return "BUFFER_SIZE_NOT_ALIGNED";
	case AUDCLNT_E_UNSUPPORTED_FORMAT:
		return "UNSUPPORTED_FORMAT";
	default:
		return "other";
	}
}

// AUTOCONVERTPCM 用の「素の」(non-EXTENSIBLE)32bit float ターゲットフォーマットを組み立てる
// (レビュー指摘 H-3)。channels/samples_per_sec は mix_format を踏襲し、ビット深度のみ float へ
// 差し替える(旧#18 と同じ方針)。
WAVEFORMATEX BuildPlainFloatFormat(const WAVEFORMATEX *mix_format)
{
	WAVEFORMATEX fmt = *mix_format;
	fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
	fmt.wBitsPerSample = 32;
	fmt.nBlockAlign = (WORD)(fmt.nChannels * (fmt.wBitsPerSample / 8));
	fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
	fmt.cbSize = 0;
	return fmt;
}

// AUTOCONVERTPCM 用の WAVEFORMATEXTENSIBLE ターゲットフォーマットを組み立てる(レビュー指摘 H-3)。
// mix_format が EXTENSIBLE または 2ch 超のとき、こちらを優先して試す(2ch 超の共有モードは
// EXTENSIBLE を要求するドライバがあるため)。dwChannelMask は元が EXTENSIBLE のときのみ継承し、
// そうでなければ 0(未指定)にする。
WAVEFORMATEXTENSIBLE BuildExtensibleFloatFormat(const WAVEFORMATEX *mix_format, bool source_is_extensible)
{
	WAVEFORMATEXTENSIBLE fmt{};
	fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	fmt.Format.nChannels = mix_format->nChannels;
	fmt.Format.nSamplesPerSec = mix_format->nSamplesPerSec;
	fmt.Format.wBitsPerSample = 32;
	fmt.Format.nBlockAlign = (WORD)(fmt.Format.nChannels * (fmt.Format.wBitsPerSample / 8));
	fmt.Format.nAvgBytesPerSec = fmt.Format.nSamplesPerSec * fmt.Format.nBlockAlign;
	fmt.Format.cbSize = 22;
	fmt.Samples.wValidBitsPerSample = 32;
	fmt.dwChannelMask =
		source_is_extensible ? reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(mix_format)->dwChannelMask : 0;
	fmt.SubFormat = kSubTypeIeeeFloat;
	return fmt;
}

// IAudioClient::IsFormatSupported(SHARED, ...) が S_OK(完全一致でサポート)を返すかだけを見る
// (レビュー指摘 H-3)。S_FALSE(近似フォーマットの提案あり)は「NG」として扱い、呼び出し側の
// フォールバックへ進める(ppClosestMatch は使わない: 素の float フォーマットへのフォールバックで
// 十分単純にカバーできるため)。
bool IsFormatAccepted(IAudioClient *client, const WAVEFORMATEX *fmt)
{
	WAVEFORMATEX *closest = nullptr;
	const HRESULT hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, fmt, &closest);
	if (closest)
		CoTaskMemFree(closest);
	return hr == S_OK;
}

// UTF-8 → wide 変換(IMMDeviceEnumerator::GetDevice() は wide 文字列を要求する。Slice 4)。
// エンドポイント ID は実質 ASCII のみだが、CP_UTF8 経由で変換して汎用に扱う。
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

WasapiOutput::~WasapiOutput()
{
	Stop();
}

bool WasapiOutput::Open(const std::string &device_id, uint32_t *out_sample_rate)
{
	// abandoned_ は一度立ったら永久に立ちっぱなし(回帰レビュー指摘 #3)。以後の Open() は
	// 何もせず即座に失敗させ、OBS 再起動を促す。
	if (abandoned_.load(std::memory_order_acquire)) {
		obs_log(LOG_ERROR, "WasapiOutput: backend is abandoned after an unresponsive render thread; "
				   "restart OBS to recover");
		return false;
	}

	// 防御(回帰レビュー指摘 #1): 呼び出し元(OutputInstance::Start())が Failed→Start 経路で
	// 先に完全停止手順を踏む設計になっているが、万一 thread_ が joinable なまま Open() が
	// 呼ばれた場合(呼び出し順序の事故)、下の `thread_ = std::thread(...)` は joinable な
	// std::thread への再代入となり std::terminate を引き起こす。ここで Stop() を挟んで
	// 確実に join・render_event_ の解放まで終わらせてから続行する。
	if (thread_.joinable())
		Stop();
	assert(render_event_ == nullptr && "WasapiOutput::Open: render_event_ must be null before (re)opening");
	if (render_event_) {
		// assert が無効化されたビルドでの最終防衛線。旧ハンドルを握ったまま次の
		// CreateEventW で上書きすると HANDLE リークになる(回帰レビュー指摘 #1)。
		CloseHandle(render_event_);
		render_event_ = nullptr;
	}

	// render thread(OpenDeviceOnRenderThread)が参照する解決済み device_id(Slice 4)。thread_ の
	// 生成前に書き込みを終えるため atomic 不要(ヘッダのコメント参照)。
	device_id_ = device_id;

	failed_.store(false, std::memory_order_relaxed);
	stop_requested_.store(false, std::memory_order_relaxed);
	start_requested_.store(false, std::memory_order_relaxed);
	counting_enabled_.store(false, std::memory_order_relaxed);
	carry_len_ = 0;
	carry_pos_ = 0;
	// ドリフト補正のセッション単位状態もここでクリアする(Slice 3)。drift_ 自体は
	// ConfigureDriftCompensator() が Open() 成功後に再構築するため、ここでは古いセッションの
	// インスタンスを持ち越さないよう先に破棄しておく。
	drift_.reset();
	last_frame_[0] = 0.0f;
	last_frame_[1] = 0.0f;
	has_last_frame_ = false;
	autoconvert_used_ = false;
	device_format_info_ = FormatInfo{};
	open_done_ = false;
	open_succeeded_.store(false, std::memory_order_relaxed);
	started_done_ = false;
	started_succeeded_ = false;

	// セッション単位の統計はここでゼロクリアする(回帰レビュー指摘 #4)。underrun/clip は
	// 前回セッションの値を引きずらない。last_hr_/last_win32_err_ も同様に新しいセッションの
	// 診断値だけを残す。
	underrun_count_.store(0, std::memory_order_relaxed);
	clip_count_.store(0, std::memory_order_relaxed);
	last_hr_.store(0, std::memory_order_relaxed);
	last_win32_err_.store(0, std::memory_order_relaxed);
	// Level Meter もセッション単位でリセットする(Slice 5: 前回セッションの残留値を新しい
	// Dock 表示に持ち越さない)。
	level_peak_l_.store(0.0f, std::memory_order_relaxed);
	level_peak_r_.store(0.0f, std::memory_order_relaxed);

	// Slice 8(遅延・NFR 計測)の計測値も前回セッションを持ち越さない。
	stream_latency_ref_time_ = 0;
	current_padding_frames_.store(0, std::memory_order_relaxed);
	current_level_frames_.store(0, std::memory_order_relaxed);
	cpu_sample_prev_cpu_100ns_ = 0;
	cpu_sample_prev_wall_ns_ = 0;
	cpu_sample_has_prev_ = false;
	render_cpu_pct_.store(0.0f, std::memory_order_relaxed);

	render_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!render_event_) {
		obs_log(LOG_ERROR, "WasapiOutput: CreateEventW failed (err=%lu)", GetLastError());
		return false;
	}

	try {
		thread_ = std::thread(&WasapiOutput::ThreadMain, this);
	} catch (const std::system_error &e) {
		obs_log(LOG_ERROR, "WasapiOutput: failed to start render thread (%s)", e.what());
		CloseHandle(render_event_);
		render_event_ = nullptr;
		return false;
	}

	std::unique_lock<std::mutex> lock(control_mutex_);
	// レビュー指摘(High-3): shutting_down_ が立っていれば(または待機中に立てば)5 秒ではなく
	// kShutdownWaitTimeout(500ms)で諦める。
	const bool signaled =
		WaitForRenderThreadHandshake(control_cv_, lock, shutting_down_, [this] { return open_done_; });
	if (!signaled) {
		lock.unlock();
		obs_log(LOG_ERROR,
			"WasapiOutput: Open() timed out waiting for render thread (backend abandoned; restart OBS "
			"to recover)");
		// 回帰レビュー指摘 #3: ここで Stop()(= join)を呼ぶと、render thread が COM 呼び出しで
		// 本当にハングしている場合に呼び出し元(UI/frontend イベントスレッド)を無期限に
		// ブロックしてしまう。join はせず、abandoned_ を立てて起床だけ試みて即座に false を返す。
		// 実際の後始末(join のポーリング/detach)は次に呼ばれる Stop() に委ねる。
		abandoned_.store(true, std::memory_order_release);
		{
			std::lock_guard<std::mutex> lk(control_mutex_);
			stop_requested_.store(true, std::memory_order_relaxed);
		}
		if (render_event_)
			SetEvent(render_event_);
		control_cv_.notify_all();
		return false;
	}
	const bool ok = open_succeeded_.load(std::memory_order_relaxed);
	lock.unlock();

	if (!ok) {
		if (thread_.joinable())
			thread_.join();
		if (render_event_) {
			CloseHandle(render_event_);
			render_event_ = nullptr;
		}
		return false;
	}

	if (out_sample_rate)
		*out_sample_rate = format_info_.sample_rate;
	return true;
}

void WasapiOutput::BindSource(RingBuffer *source)
{
	// Open() 成功後・Start() 前に 1 回だけ呼ばれる想定(control スレッド、RT ではない)。
	// source->MaxChunkFrames() に合わせて carry buffer を確保する(レビュー指摘 #2)。
	source_ = source;
	const uint32_t max_chunk_frames = source ? source->MaxChunkFrames() : 0;
	carry_buf_ = max_chunk_frames > 0 ? std::make_unique<float[]>((size_t)max_chunk_frames * RingBuffer::kChannels)
					  : nullptr;
	carry_capacity_ = max_chunk_frames;
	carry_len_ = 0;
	carry_pos_ = 0;
}

void WasapiOutput::ConfigureDriftCompensator(double target_level_frames, double frames_per_chunk,
					     double capacity_frames, double device_sample_rate_hz)
{
	// Open() 成功後・Start() 前に 1 回だけ呼ばれる想定(control スレッド、BindSource() と同じ規約)。
	// render thread はこの時点ではまだ start_requested_ 待ちで RenderLoop に入っていないため、
	// drift_ をここで construct してもロック不要(レビュー指摘なし、BindSource() の source_ 代入と
	// 同じ考え方)。
	drift_ = std::make_unique<DriftCompensator>(target_level_frames, frames_per_chunk, capacity_frames,
						    device_sample_rate_hz);
	drift_frames_per_chunk_ = frames_per_chunk;
}

bool WasapiOutput::Start()
{
	if (abandoned_.load(std::memory_order_acquire)) {
		obs_log(LOG_ERROR, "WasapiOutput: backend is abandoned after an unresponsive render thread; "
				   "restart OBS to recover");
		return false;
	}

	if (!thread_.joinable() || !open_succeeded_.load(std::memory_order_relaxed))
		return false;

	{
		// 述語に使う状態の store は control_mutex_ のロック下で行う(レビュー指摘 #1)。
		std::lock_guard<std::mutex> lock(control_mutex_);
		start_requested_.store(true, std::memory_order_relaxed);
	}
	control_cv_.notify_all(); // notify はロック解放後でよい。

	std::unique_lock<std::mutex> lock(control_mutex_);
	// レビュー指摘(High-3): Open() と同じ理由で shutting_down_ 時は短縮タイムアウトへ切り替える。
	const bool signaled =
		WaitForRenderThreadHandshake(control_cv_, lock, shutting_down_, [this] { return started_done_; });
	if (!signaled) {
		lock.unlock();
		obs_log(LOG_ERROR,
			"WasapiOutput: Start() timed out waiting for render thread (backend abandoned; restart OBS "
			"to recover)");
		// Open() のタイムアウトと同じ理由(回帰レビュー指摘 #3)で join せず見捨てる。
		abandoned_.store(true, std::memory_order_release);
		{
			std::lock_guard<std::mutex> lk(control_mutex_);
			stop_requested_.store(true, std::memory_order_relaxed);
		}
		if (render_event_)
			SetEvent(render_event_);
		control_cv_.notify_all();
		return false;
	}
	return started_succeeded_;
}

void WasapiOutput::Stop()
{
	{
		// 述語に使う状態の store は control_mutex_ のロック下で行う(レビュー指摘 #1)。
		std::lock_guard<std::mutex> lock(control_mutex_);
		stop_requested_.store(true, std::memory_order_relaxed);
	}
	if (render_event_)
		SetEvent(render_event_); // WaitForSingleObject を起こす(停止手順、wasapi-com.md)。ロック外で可。
	control_cv_.notify_all();        // Open()/Start() 待ちで cv.wait 中のスレッドを起こす(ロック解放後)。

	// 通常経路(abandoned_ == false)では従来どおり join する(呼び出し元は必ず有限時間で
	// 抜けられる正常系のみ)。abandoned_ == true のときだけ有限時間のポーリング+detach に切り替える
	// (回帰レビュー指摘 #3)。
	//
	// Slice 6(S6-1、旧コメントの「detach 後、その this を次の Open() で再利用する」設計を廃止):
	// detach した場合は thread_detached_ を立てて呼び出し元(OutputInstance の control thread)に
	// 知らせる。呼び出し元は RequiresZombieRetention()==true のこのインスタンスを二度と
	// Open()/Start() せず、delete もしない(所有権ごとゾンビリストへ移し、プロセス終了まで保持する。
	// ARCHITECTURE.md §13 S6-1)。detach 後、その render thread は abandoned_ に至った時点でまだ
	// RenderLoop(RT ループ)に入っていない(OpenDeviceOnRenderThread の COM 呼び出し、または
	// audio_client_->Start() 待ちで stuck)ため、audio データパス(RingBuffer/carry_buf_)には
	// 触れない。render_event_ の CloseHandle や COM リソースの Release は detach 後は行わない
	// (見捨てたスレッドがまだ握っている可能性があるため、リークを許容する。プロセス終了時に OS が
	// 回収する最終手段)。この設計により、見捨てたスレッドが後から this を参照しても、this 自体は
	// (呼び出し元がゾンビリストで生かし続ける限り)有効なメモリのままなので UAF にはならない
	// (旧来の「新しい Open() が同じ this を再利用する」経路こそが UAF の原因だったため、その経路を
	// 削除したことが本質的な修正)。
	bool detached = false;
	if (thread_.joinable()) {
		if (abandoned_.load(std::memory_order_acquire)) {
			const DWORD wait_ms =
				(DWORD)std::chrono::duration_cast<std::chrono::milliseconds>(kAbandonedJoinWaitTimeout)
					.count();
			const DWORD wait_result = WaitForSingleObject(thread_.native_handle(), wait_ms);
			if (wait_result == WAIT_OBJECT_0) {
				// render thread は実は既に終了していた。join は即座に完了する。
				thread_.join();
			} else {
				obs_log(LOG_ERROR,
					"WasapiOutput: abandoned render thread still unresponsive after %lld s; "
					"detaching and retaining this instance forever (zombie; a new WasapiOutput "
					"will be used to continue)",
					(long long)std::chrono::duration_cast<std::chrono::seconds>(
						kAbandonedJoinWaitTimeout)
						.count());
				thread_.detach();
				detached = true;
				thread_detached_.store(true, std::memory_order_release);
			}
		} else {
			thread_.join();
			obs_log(LOG_INFO, "WasapiOutput: render thread joined");
		}
	}

	if (!detached && render_event_) {
		CloseHandle(render_event_);
		render_event_ = nullptr;
	}

	source_ = nullptr;

	// レビュー指摘 M-3: 停止直後に前セッションのメーター値を残さない。Open() でも同様にリセットして
	// いるが(次回 Open() まで)、Stop() 直後〜次の Open() までの間に Dock の QTimer が
	// LevelPeakL()/R() を読む可能性があるため、ここでも明示的にゼロへ戻す(render thread は既に
	// join/detach 済みで level_peak_* を書かないため、この store と競合しない)。
	level_peak_l_.store(0.0f, std::memory_order_relaxed);
	level_peak_r_.store(0.0f, std::memory_order_relaxed);
}

void WasapiOutput::ThreadMain()
{
	// 専用 render thread の先頭で CoInitializeEx する(wasapi-com.md)。RPC_E_CHANGED_MODE は
	// 「既に別モデルで初期化済み」を意味し COM 自体は利用可能なので成功扱いとするが、
	// その場合は自分が初期化した扱いにせず CoUninitialize を呼ばない。
	const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool co_owned = SUCCEEDED(co_hr);
	const bool co_usable = SUCCEEDED(co_hr) || co_hr == RPC_E_CHANGED_MODE;

	const bool open_ok = co_usable && OpenDeviceOnRenderThread();
	if (!co_usable) {
		obs_log(LOG_ERROR, "WasapiOutput: CoInitializeEx failed (hr=0x%08lX)", co_hr);
	}

	{
		std::lock_guard<std::mutex> lock(control_mutex_);
		open_succeeded_.store(open_ok, std::memory_order_relaxed);
		open_done_ = true;
	}
	control_cv_.notify_all();

	if (!open_ok) {
		ReleaseDeviceResources();
		if (co_owned)
			CoUninitialize();
		return;
	}

	{
		std::unique_lock<std::mutex> lock(control_mutex_);
		control_cv_.wait(lock, [this] {
			return start_requested_.load(std::memory_order_relaxed) ||
			       stop_requested_.load(std::memory_order_relaxed);
		});
	}

	if (!start_requested_.load(std::memory_order_relaxed)) {
		// Stop() のみが呼ばれ Start() は呼ばれなかった(開通のみで終了)。
		ReleaseDeviceResources();
		if (co_owned)
			CoUninitialize();
		return;
	}

	DWORD mmcss_task_index = 0;
	HANDLE mmcss_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task_index);
	if (!mmcss_handle) {
		obs_log(LOG_WARNING, "WasapiOutput: AvSetMmThreadCharacteristics failed (err=%lu); continuing",
			GetLastError());
	}

	const HRESULT start_hr = audio_client_->Start();
	const bool start_ok = SUCCEEDED(start_hr);
	if (!start_ok) {
		failed_.store(true, std::memory_order_release);
		last_hr_.store(start_hr, std::memory_order_relaxed);
	}

	{
		std::lock_guard<std::mutex> lock(control_mutex_);
		started_succeeded_ = start_ok;
		started_done_ = true;
	}
	control_cv_.notify_all();

	if (start_ok) {
		RenderLoop();

		const HRESULT stop_hr = audio_client_->Stop();
		if (FAILED(stop_hr)) {
			// レビュー指摘 #12: Stop() の HRESULT もログに残す(致命扱いにはしない)。
			obs_log(LOG_WARNING, "WasapiOutput: IAudioClient::Stop failed (hr=0x%08lX)", stop_hr);
		}

		if (failed_.load(std::memory_order_acquire)) {
			const long hr_val = last_hr_.load(std::memory_order_relaxed);
			obs_log(LOG_ERROR,
				"WasapiOutput: render loop stopped due to error (hr=0x%08lX, category=%s, win32_err=%lu)",
				(unsigned long)hr_val, ClassifyHr((HRESULT)hr_val),
				last_win32_err_.load(std::memory_order_relaxed));
		}
	} else {
		obs_log(LOG_ERROR, "WasapiOutput: IAudioClient::Start failed (hr=0x%08lX, category=%s)", start_hr,
			ClassifyHr(start_hr));
	}

	if (mmcss_handle)
		AvRevertMmThreadCharacteristics(mmcss_handle);

	ReleaseDeviceResources();
	if (co_owned)
		CoUninitialize();
}

bool WasapiOutput::OpenDeviceOnRenderThread()
{
	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
				      (void **)enumerator_.GetAddressOf());
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "WasapiOutput: CoCreateInstance(MMDeviceEnumerator) failed (hr=0x%08lX)", hr);
		return false;
	}

	// device_id_ の解決(Slice 4、D-14)。"default"(または空文字列: 呼び出し元の防御的既定値)は
	// role eConsole の既定デバイス、それ以外は解決済み endpoint id をそのまま GetDevice() に渡す。
	// id 自体の突き合わせ(保存値 vs 列挙結果)は device-matcher/OutputInstance の責務で、ここでは
	// 行わない。
	if (device_id_.empty() || device_id_ == "default") {
		hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, device_.GetAddressOf());
		if (FAILED(hr)) {
			obs_log(LOG_ERROR, "WasapiOutput: GetDefaultAudioEndpoint failed (hr=0x%08lX)", hr);
			return false;
		}
	} else {
		// ここに渡ってくる device_id_ は device-matcher/OutputInstance が保存値と列挙結果を
		// 突き合わせ済みの id(または "default")のみ(レビュー指摘 L-8)。未解決 id 向けの案内
		// ('List Devices (Debug)' を実行させるメッセージ)は OutputInstance::Start() 側
		// (device_matcher::Resolve 失敗時)が出すため、ここでは出さない。それでも GetDevice が
		// 失敗するのは、列挙から Open() までの間にデバイスが切断された等ごく短い競合のみなので、
		// 何が起きたかを簡潔に記録するだけにとどめる。
		const std::wstring device_id_wide = Utf8ToWide(device_id_);
		hr = enumerator_->GetDevice(device_id_wide.c_str(), device_.GetAddressOf());
		if (FAILED(hr)) {
			obs_log(LOG_ERROR, "WasapiOutput: GetDevice(id=%s) failed (hr=0x%08lX, category=%s)",
				device_id_.c_str(), hr, ClassifyHr(hr));
			return false;
		}
	}

	hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)audio_client_.GetAddressOf());
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "WasapiOutput: IMMDevice::Activate(IAudioClient) failed (hr=0x%08lX)", hr);
		return false;
	}

	WAVEFORMATEX *mix_format = nullptr;
	hr = audio_client_->GetMixFormat(&mix_format);
	if (FAILED(hr) || !mix_format) {
		obs_log(LOG_ERROR, "WasapiOutput: GetMixFormat failed (hr=0x%08lX)", hr);
		return false;
	}

	const bool source_is_float = IsFloatFormat(mix_format);
	const bool source_is_extensible = mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix_format->cbSize >= 22;

	// 開始時ダンプ向けにデバイスの生フォーマット(変換前)を記録する(再レビュー指摘 M)。
	// この後の Initialize でストリーム形式(format_info_)が別物になり得る(AUTOCONVERTPCM)。
	device_format_info_.sample_rate = mix_format->nSamplesPerSec;
	device_format_info_.channels = mix_format->nChannels;
	device_format_info_.bits_per_sample = mix_format->wBitsPerSample;
	device_format_info_.format_tag = mix_format->wFormatTag;
	device_format_info_.is_float = source_is_float;

	obs_log(LOG_INFO,
		"WasapiOutput: device mix format tag=%u channels=%u samples_per_sec=%u bits_per_sample=%u is_float=%d",
		mix_format->wFormatTag, mix_format->nChannels, mix_format->nSamplesPerSec, mix_format->wBitsPerSample,
		source_is_float ? 1 : 0);

	// フォーマット方針(ブリーフ実装内容 6/7、ARCHITECTURE.md §5 手順 2.5): channel 数の交渉はしない
	// (2ch 超は先頭 2ch に L/R、残り 0 埋め。RenderLoop/WriteFrames 側で対応)。1ch 以下は Failed。
	if (mix_format->nChannels < 2) {
		obs_log(LOG_ERROR, "WasapiOutput: unsupported mix format (need >=2ch); see dump above");
		CoTaskMemFree(mix_format);
		return false;
	}

	// 非 float(例: 16bit PCM)デバイスは、独自の PCM 変換を実装せず WASAPI 自身の変換に委ねる
	// (#18、旧#18: AUTOCONVERTPCM×EVENTCALLBACK 併用は API 層で制限なしと部分確認済み)。
	// 具体的には「アプリ側が書き込むフォーマット」として IEEE float(チャンネル数・サンプルレートは
	// デバイスの GetMixFormat() を踏襲、ビット深度のみ 32bit float に差し替え)を明示的に指定し、
	// AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | SRC_DEFAULT_QUALITY を付けて Initialize する。これにより
	// WriteFrames()/ZeroFillFrames() は Slice 2 から変更なしで float 書き込みを続けられる
	// (デバイス実体のビット深度変換はオーディオエンジン内部に委ねる)。
	//
	// 未確認事項(実機で最終確認する。旧#18 は「部分確認」のまま): AUTOCONVERTPCM が本来
	// 意図する用途は MS Learn の記述だけでは断定できない(サンプルレート変換専用なのか、
	// ビット深度も含む変換に使えるのか)。SDL2(src/audio/wasapi/SDL_wasapi.c)がアプリ側の
	// 希望フォーマットで Initialize しつつ AUTOCONVERTPCM を付ける実装になっていることを根拠に
	// 同じ流儀を採用したが、実機での無音/ノイズ有無の確認は本 Slice の完了条件(実機 30 分)で
	// 行うこと。
	//
	// 多ch(2ch 超)/EXTENSIBLE のとき(レビュー指摘 H-3): 一部ドライバは共有モードで
	// WAVEFORMATEXTENSIBLE を要求するため、まず EXTENSIBLE ターゲットを組み立てて
	// IsFormatSupported で事前検証する。NG、または Initialize 自体が失敗したら、「素の」
	// (non-EXTENSIBLE)float フォーマットで 1 回だけリトライする。それも失敗したら Failed
	// (独自 PCM 変換の実装はスコープ外のまま)。
	WAVEFORMATEX final_fmt{};
	bool init_ok = false;
	bool used_autoconvert = false;
	bool used_extensible = false;

	if (source_is_float) {
		hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0,
					       mix_format, nullptr);
		if (SUCCEEDED(hr)) {
			init_ok = true;
			final_fmt = *mix_format;
		}
	} else {
		const bool need_extensible = source_is_extensible || mix_format->nChannels > 2;
		const WAVEFORMATEX plain_fmt = BuildPlainFloatFormat(mix_format);
		constexpr DWORD kAutoconvertFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
						    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
						    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

		if (need_extensible) {
			const WAVEFORMATEXTENSIBLE ext_fmt =
				BuildExtensibleFloatFormat(mix_format, source_is_extensible);
			const WAVEFORMATEX *ext_fmt_ptr = reinterpret_cast<const WAVEFORMATEX *>(&ext_fmt);
			if (IsFormatAccepted(audio_client_.Get(), ext_fmt_ptr)) {
				hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, kAutoconvertFlags, 0, 0,
							       ext_fmt_ptr, nullptr);
				if (SUCCEEDED(hr)) {
					init_ok = true;
					used_autoconvert = true;
					used_extensible = true;
					final_fmt = ext_fmt.Format;
				} else {
					obs_log(LOG_WARNING,
						"WasapiOutput: Initialize with EXTENSIBLE AUTOCONVERTPCM target failed "
						"(hr=0x%08lX); retrying with a plain float target",
						hr);
				}
			} else {
				obs_log(LOG_WARNING,
					"WasapiOutput: IsFormatSupported rejected the EXTENSIBLE AUTOCONVERTPCM "
					"target format; retrying with a plain float target");
			}
		}

		if (!init_ok) {
			hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, kAutoconvertFlags, 0, 0, &plain_fmt,
						       nullptr);
			if (SUCCEEDED(hr)) {
				init_ok = true;
				used_autoconvert = true;
				used_extensible = false;
				final_fmt = plain_fmt;
			}
		}
	}

	CoTaskMemFree(mix_format);
	mix_format = nullptr;

	if (!init_ok) {
		obs_log(LOG_ERROR,
			"WasapiOutput: IAudioClient::Initialize failed (hr=0x%08lX, category=%s, autoconvert_attempted=%d)",
			hr, ClassifyHr(hr), source_is_float ? 0 : 1);
		return false;
	}

	autoconvert_used_ = used_autoconvert;
	// format_info_(開始時ダンプ向け)には実際に Initialize したフォーマットを反映する
	// (GetMixFormat() が返した生のデバイス値ではない。レビュー指摘 H-3)。
	format_info_.sample_rate = final_fmt.nSamplesPerSec;
	format_info_.channels = final_fmt.nChannels;
	format_info_.bits_per_sample = final_fmt.wBitsPerSample;
	format_info_.format_tag = final_fmt.wFormatTag;
	format_info_.is_float = true; // Initialize 成功時は素の float か AUTOCONVERTPCM 経由の float。

	if (used_autoconvert)
		obs_log(LOG_INFO,
			"WasapiOutput: Initialize succeeded using AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM (%s target format)",
			used_extensible ? "extensible" : "plain");

	// SetEventHandle は Start() 前に必須(wasapi-com.md)。
	hr = audio_client_->SetEventHandle(render_event_);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "WasapiOutput: SetEventHandle failed (hr=0x%08lX)", hr);
		return false;
	}

	hr = audio_client_->GetBufferSize(&buffer_frame_count_);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "WasapiOutput: GetBufferSize failed (hr=0x%08lX)", hr);
		return false;
	}

	hr = audio_client_->GetService(__uuidof(IAudioRenderClient), (void **)render_client_.GetAddressOf());
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "WasapiOutput: GetService(IAudioRenderClient) failed (hr=0x%08lX)", hr);
		return false;
	}

	// Slice 8(AC-11、実装内容の要点 1/4): GetStreamLatency はセットアップ専用フェーズ(RT ループの外)
	// で 1 回だけ取得する。失敗しても致命的にはしない(est_latency_ms からこの項が抜けるだけ)。
	REFERENCE_TIME stream_latency = 0;
	hr = audio_client_->GetStreamLatency(&stream_latency);
	if (SUCCEEDED(hr)) {
		stream_latency_ref_time_ = stream_latency;
	} else {
		obs_log(LOG_WARNING,
			"WasapiOutput: GetStreamLatency failed (hr=0x%08lX); est_latency_ms will omit "
			"the device stream latency term",
			hr);
	}

	return true;
}

void WasapiOutput::RenderLoop()
{
	// ここから先は RT ループ: ヒープ確保・ロック・blog・COM 新規呼び出し(Release 系は Stop 手順内)
	// 禁止(rt-audio.md)。エラーは atomic フラグへ記録して break するのみ。ログは呼び出し元
	// (ThreadMain、ループを抜けた後)が行う。
	while (!stop_requested_.load(std::memory_order_relaxed)) {
		const DWORD wait_result = WaitForSingleObject(render_event_, 2000);
		if (stop_requested_.load(std::memory_order_relaxed))
			break;

		if (wait_result == WAIT_TIMEOUT) {
			// 2 秒無通知は異常だが再接続は S6 で実装予定(ブリーフ実装内容 12)。stop 要求の
			// 再チェックのみ行い、致命扱いにはしない。
			continue;
		}
		if (wait_result != WAIT_OBJECT_0) {
			last_win32_err_.store(GetLastError(), std::memory_order_relaxed);
			failed_.store(true, std::memory_order_release);
			break;
		}

		UINT32 padding = 0;
		HRESULT hr = audio_client_->GetCurrentPadding(&padding);
		if (FAILED(hr)) {
			last_hr_.store(hr, std::memory_order_relaxed);
			failed_.store(true, std::memory_order_release);
			break;
		}
		// Slice 8(est_latency_ms の「WASAPI バッファ滞留」項): atomic store のみ、RT 安全。
		current_padding_frames_.store(padding, std::memory_order_relaxed);

		const UINT32 available = (buffer_frame_count_ > padding) ? (buffer_frame_count_ - padding) : 0;
		if (available == 0)
			continue;

		// ドリフト補正の決定はこの wake で 1 回だけ行う(ブリーフ実装内容 1〜3、7)。水位は pop 前に
		// 読む(RingBuffer::Level() は atomic load のみで RT 安全)。チャンク整数ではなく
		// 「frames 実効水位」(pop 済みチャンク数 * frames_per_chunk + carry の残り frames)を渡す
		// (レビュー指摘 H-1: チャンク単位だと発動閾値が実容量に対して粗すぎる)。
		// Slice 8 レビュー Medium-3: この値は診断スレッドの est_latency_ms 計算にもそのまま使うため、
		// drift_ の有無に関わらず source_ さえあれば毎 wake 計算して atomic store する
		// (CurrentLevelFrames() 参照。ring_buffer->Level() を診断スレッド側で再計算する旧近似の
		// 誤差(carry 分を含まない、最大 1 チャンク分)を解消する)。
		double current_level_frames = 0.0;
		if (source_)
			current_level_frames =
				(double)source_->Level() * drift_frames_per_chunk_ + (double)(carry_len_ - carry_pos_);
		current_level_frames_.store(current_level_frames > 0.0 ? (uint32_t)(current_level_frames + 0.5) : 0u,
					    std::memory_order_relaxed);

		DriftCompensator::Decision drift_decision = DriftCompensator::Decision::None;
		if (drift_ && source_)
			drift_decision = drift_->OnWake(current_level_frames, available);
		bool drift_applied = false; // この wake で挿入/間引きを既に適用したか(1 wake につき最大 1 回)

		BYTE *device_buffer = nullptr;
		hr = render_client_->GetBuffer(available, &device_buffer);
		if (FAILED(hr)) {
			last_hr_.store(hr, std::memory_order_relaxed);
			failed_.store(true, std::memory_order_release);
			break;
		}

		uint32_t frames_written = 0;
		bool underrun_this_wake = false;
		// Level Meter(D-15): この wake で書き込んだ全フレームのうちの L/R 絶対値ピーク
		// (クランプ後、線形 0.0〜1.0)。WriteFrames() 呼び出しをまたいでローカルに集計し、
		// この wake の終わりで 1 回だけ atomic store する(rt-audio.md、ヒープ確保なし)。
		float wake_peak_l = 0.0f;
		float wake_peak_r = 0.0f;
		while (frames_written < available) {
			// 挿入: 直前フレーム(last_frame_)を複製してもう 1 回書く。ソースを消費しないため
			// carry_len_/carry_pos_ の会計には触れない(ブリーフ実装内容 4)。
			if (drift_decision == DriftCompensator::Decision::Insert && !drift_applied && has_last_frame_) {
				WriteFrames(device_buffer +
						    (size_t)frames_written * format_info_.channels * sizeof(float),
					    last_frame_, 1, &wake_peak_l, &wake_peak_r);
				frames_written += 1;
				drift_applied = true;
				continue;
			}

			if (carry_len_ == 0) {
				if (!source_->TryPop(carry_buf_.get(), &carry_len_)) {
					underrun_this_wake = true;
					break;
				}
				// carry_buf_ は BindSource() で carry_capacity_ frames 分を確保済み。
				// RingBuffer 側の TryPush/PushSilence が max_chunk_frames_ で切り詰める
				// ため、通常はここを超えないはず。RT ループ内で assert()(失敗時に I/O を伴う
				// abort/stderr 出力をする)は使わず、Debug ビルドに限定した非 I/O チェックで
				// 最終防衛する(レビュー指摘 L-7)。
#ifndef NDEBUG
				if (carry_len_ > carry_capacity_)
					carry_len_ = carry_capacity_;
#endif
				carry_pos_ = 0;
			}

			const uint32_t remain_in_chunk = carry_len_ - carry_pos_;
			const uint32_t remain_in_device = available - frames_written;
			uint32_t to_write = remain_in_chunk < remain_in_device ? remain_in_chunk : remain_in_device;

			// 間引き: pop したチャンクの残りをちょうど書き切るタイミング(チャンク境界)でのみ、
			// 末尾 1 frame を書かずに読み飛ばす(carry_pos_ を to_write より 1 多く進めることで
			// ソースを 1 frame 多く消費する。ブリーフ実装内容 4)。チャンクをまたいで累積させない
			// ため、この wake で 1 回だけ適用する。
			bool drop_this_iteration = false;
			if (drift_decision == DriftCompensator::Decision::Drop && !drift_applied &&
			    to_write == remain_in_chunk && to_write > 0) {
				to_write -= 1;
				drop_this_iteration = true;
				drift_applied = true;
			}

			if (to_write > 0) {
				WriteFrames(device_buffer +
						    (size_t)frames_written * format_info_.channels * sizeof(float),
					    carry_buf_.get() + (size_t)carry_pos_ * RingBuffer::kChannels, to_write,
					    &wake_peak_l, &wake_peak_r);
				// 挿入用に直近で書いた 1 frame(L, R)を保存する(clamp 前の生値でよい。
				// WriteFrames が複製時にも clamp するため)。
				last_frame_[0] =
					carry_buf_[((size_t)carry_pos_ + to_write - 1) * RingBuffer::kChannels + 0];
				last_frame_[1] =
					carry_buf_[((size_t)carry_pos_ + to_write - 1) * RingBuffer::kChannels + 1];
				has_last_frame_ = true;
			}

			carry_pos_ += drop_this_iteration ? (to_write + 1) : to_write;
			frames_written += to_write;
			if (carry_pos_ >= carry_len_) {
				carry_len_ = 0;
				carry_pos_ = 0;
			}
		}

		// Level Meter: この wake で書き込んだ実データの L/R ピークを 1 回だけ store する(D-15)。
		// 何も書けなかった wake(underrun で frames_written==0)は wake_peak_l/r が初期値 0.0f の
		// ままなので、無音として反映される(実態と一致)。
		level_peak_l_.store(wake_peak_l, std::memory_order_relaxed);
		level_peak_r_.store(wake_peak_r, std::memory_order_relaxed);

		// 統計加算/レート制限カウンタのリセットは、実際に適用できた wake でのみ行う
		// (レビュー指摘 H-2)。drift_applied が false のまま(挿入できる last_frame_ が無い、
		// または間引きに使えるチャンク境界がこの wake 内に来なかった等)なら NotifyApplied() を
		// 呼ばない。frames_since_last_correction_ はリセットされないままなので、次 wake の
		// OnWake() が同じ決定を返し続け、自動的に再挑戦になる。
		// 加えて、この wake で underrun も起きていた場合(再レビュー指摘 L)は drift_ins/drift_drop
		// に計上しない: Insert の直前フレーム複製は、この場合ドリフト補正の成果ではなく
		// underrun によるゼロ埋め直前の穴埋めに過ぎず、正常時の補正と同列に数えると統計が
		// 実態より水増しされる。NotifyApplied() を呼ばないため frames_since_last_correction_ も
		// リセットされず、underrun が解消した wake で改めて正しく計上される。
		if (drift_ && drift_applied && !underrun_this_wake)
			drift_->NotifyApplied(drift_decision);

		if (frames_written < available) {
			ZeroFillFrames(device_buffer + (size_t)frames_written * format_info_.channels * sizeof(float),
				       available - frames_written);
		}

		if (underrun_this_wake && counting_enabled_.load(std::memory_order_relaxed))
			underrun_count_.fetch_add(1, std::memory_order_relaxed);

		hr = render_client_->ReleaseBuffer(available, 0);
		if (FAILED(hr)) {
			last_hr_.store(hr, std::memory_order_relaxed);
			failed_.store(true, std::memory_order_release);
			break;
		}
	}
}

void WasapiOutput::WriteFrames(BYTE *dst, const float *stereo_src, uint32_t frames, float *peak_l_accum,
			       float *peak_r_accum)
{
	const uint16_t channels = format_info_.channels;
	auto *out = reinterpret_cast<float *>(dst);
	// per-wake でローカル集計してから 1 回だけ atomic 加算する(レビュー指摘 #13: クリップした
	// サンプル数を数える。イベント回数ではない)。
	uint32_t clipped_samples = 0;
	// Level Meter(D-15): クランプ後の絶対値ピークを呼び出し元の wake ローカル変数へ蓄積する
	// (ヒープ確保・atomic 操作なし。呼び出し元が wake の最後で 1 回だけ atomic store する)。
	float local_peak_l = *peak_l_accum;
	float local_peak_r = *peak_r_accum;

	for (uint32_t i = 0; i < frames; ++i) {
		float l = stereo_src[i * 2 + 0];
		float r = stereo_src[i * 2 + 1];
		if (l > 1.0f) {
			l = 1.0f;
			++clipped_samples;
		} else if (l < -1.0f) {
			l = -1.0f;
			++clipped_samples;
		}
		if (r > 1.0f) {
			r = 1.0f;
			++clipped_samples;
		} else if (r < -1.0f) {
			r = -1.0f;
			++clipped_samples;
		}

		const float abs_l = l < 0.0f ? -l : l;
		const float abs_r = r < 0.0f ? -r : r;
		if (abs_l > local_peak_l)
			local_peak_l = abs_l;
		if (abs_r > local_peak_r)
			local_peak_r = abs_r;

		out[i * channels + 0] = l;
		if (channels > 1)
			out[i * channels + 1] = r;
		for (uint16_t c = 2; c < channels; ++c)
			out[i * channels + c] = 0.0f;
	}

	*peak_l_accum = local_peak_l;
	*peak_r_accum = local_peak_r;

	if (clipped_samples > 0)
		clip_count_.fetch_add(clipped_samples, std::memory_order_relaxed);
}

void WasapiOutput::ZeroFillFrames(BYTE *dst, uint32_t frames)
{
	std::memset(dst, 0, (size_t)frames * format_info_.channels * sizeof(float));
}

void WasapiOutput::ReleaseDeviceResources()
{
	render_client_.Reset();
	audio_client_.Reset();
	device_.Reset();
	enumerator_.Reset();
	buffer_frame_count_ = 0;
	obs_log(LOG_INFO, "WasapiOutput: WASAPI/COM resources released");
}

void WasapiOutput::SampleCpuUsage()
{
	// rt-audio.md/ブリーフ実装内容 4: GetThreadTimes は RT ループ(RenderLoop)の外で行う。呼び出し元は
	// DiagnosticsLogger の周期スレッド(2 秒ごと、RT ではない)のみを想定する。render thread が
	// joinable でなければ(未開始、または既に join/detach 済み)何もしない。render_cpu_pct_ は直近の
	// 値を保持したままになる(停止直後のサマリで「最後に計測できた値」を出すのと同じ考え方)。
	if (!thread_.joinable())
		return;

	FILETIME creation_time{}, exit_time{}, kernel_time{}, user_time{};
	if (!GetThreadTimes(thread_.native_handle(), &creation_time, &exit_time, &kernel_time, &user_time))
		return;

	const auto to_100ns = [](const FILETIME &ft) -> uint64_t {
		return ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
	};
	const uint64_t cpu_100ns = to_100ns(kernel_time) + to_100ns(user_time);
	const uint64_t now_ns = os_gettime_ns();

	// 初回サンプルは差分が取れないため基準値を記録するだけで終わる(C-08: ウィンドウ境界の扱い。
	// 2 回目以降のサンプルから実際の経過時間ベースで% を出す)。
	if (cpu_sample_has_prev_) {
		const uint64_t wall_delta_ns = now_ns - cpu_sample_prev_wall_ns_;
		if (wall_delta_ns > 0 && cpu_100ns >= cpu_sample_prev_cpu_100ns_) {
			const uint64_t cpu_delta_ns = (cpu_100ns - cpu_sample_prev_cpu_100ns_) * 100ULL;
			const double pct = (double)cpu_delta_ns / (double)wall_delta_ns * 100.0;
			render_cpu_pct_.store((float)pct, std::memory_order_relaxed);
		}
	}
	cpu_sample_prev_cpu_100ns_ = cpu_100ns;
	cpu_sample_prev_wall_ns_ = now_ns;
	cpu_sample_has_prev_ = true;
}
