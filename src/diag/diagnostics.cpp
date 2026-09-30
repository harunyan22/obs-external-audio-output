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

// 対応 REQ: REQ-LOG-001, REQ-AUDIO-005, REQ-UI-008 / Slice 2, Slice 3, Slice 5

#include "diagnostics.hpp"

#include "../audio/audio-mix-source.hpp"
#include "../audio/ring-buffer.hpp"
#include "../backend/windows/wasapi-output.hpp"

#include <obs-module.h>
#include <obs.h>
#include <plugin-support.h>

#include <windows.h>
#include <winternl.h>

#include <chrono>
#include <cmath>
#include <cstdio>

namespace {

constexpr auto kLogInterval = std::chrono::seconds(2);
// 無音(0.0 linear)を表現するための下限。log10(0) を避けるためのフロア値。
constexpr float kSilenceFloorDbfs = -100.0f;

// RtlGetVersion の関数ポインタ型(<winternl.h> は宣言のみで実体をエクスポートしないため、
// ntdll.dll から動的に取得する。GetVersionEx はアプリマニフェストの互換シムの影響を受けるため
// 使わない。診断ログ/Dock コピー向けの情報取得であり RT ではない)。
using RtlGetVersionFn = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);

// Windows 11 は内部的には dwMajorVersion=10 のまま、ビルド番号のみで判別できる(Microsoft Learn、
// 22000 以降が Windows 11)。
constexpr DWORD kWindows11MinBuildNumber = 22000;

float LinearToDbfs(float linear)
{
	if (linear <= 0.0f)
		return kSilenceFloorDbfs;
	float db = 20.0f * std::log10(linear);
	return db < kSilenceFloorDbfs ? kSilenceFloorDbfs : db;
}

// WasapiOutput::DriftLevelEma() は drift_ 未構築時に NaN を返す(レビュー指摘 Medium-2)。
// ログでは 0.0 と紛らわしくないよう "n/a" と表示する。バッファは呼び出し元のローカル変数
// (obs_log の可変長引数へ渡す間だけ生存すればよい)。
void FormatLevelEma(double level_ema, char *out, size_t out_size)
{
	if (std::isnan(level_ema))
		snprintf(out, out_size, "n/a");
	else
		snprintf(out, out_size, "%.2f", level_ema);
}

// Slice 8(AC-11、実装内容の要点 1): 推定遅延 = リング滞留 + WASAPI バッファ滞留(padding) +
// GetStreamLatency。
//
// レビュー指摘 Medium-3(旧近似の解消): 以前は「リング実効 frames」を診断スレッド側で
// ring_buffer->Level()*frames_per_chunk として再計算していたが、carry_len_/carry_pos_(render thread
// 専有の非 atomic 状態)を診断スレッドから安全に読めないため carry 分が欠落し、誤差が最大 1 チャンク分
// (約 1024 frames をデバイスレート換算、48kHz で ≒21ms)あった。WasapiOutput::RenderLoop が毎 wake で
// 計算済みの値(carry 込み)を CurrentLevelFrames() として atomic 公開するようになったため、ここでは
// それをそのまま使う(近似ではなく RenderLoop 自身の計算結果)。
//
// レビュー指摘 Medium-6(二重計上の明記): 3 項(lat_ring_ms/lat_pad_ms/lat_stream_ms)はそれぞれ
// 「リングバッファに滞留中の frames」「WASAPI デバイスバッファに既に渡り再生待ちの frames」
// 「IAudioClient::GetStreamLatency() が報告するデバイス内部の追加レイテンシ」を指しており、単純合算
// (TotalMs())は実際の再生遅延の「上界推定」である(3 項が表す時間区間が完全に排他的である保証はなく、
// GetStreamLatency() の定義がデバイス/ドライバ依存で padding と一部重複しうるため。README
// (tools/measure/README.md)にも明記する)。device_sample_rate が 0(Open 未完了)の間は valid=false
// (呼び出し元は "n/a" として扱うこと。レビュー指摘 Medium-5: 停止時に 0.0 という数値を残留値として
// 出さない)。
struct EstLatencyBreakdown {
	bool valid = false;
	double ring_ms = 0.0;
	double pad_ms = 0.0;
	double stream_ms = 0.0;
	double TotalMs() const { return ring_ms + pad_ms + stream_ms; }
};

EstLatencyBreakdown ComputeEstLatencyBreakdown(const WasapiOutput *backend)
{
	EstLatencyBreakdown result;
	const uint32_t device_rate = backend->GetFormatInfo().sample_rate;
	if (device_rate == 0)
		return result; // valid=false のまま。
	result.valid = true;
	result.ring_ms = (double)backend->CurrentLevelFrames() / (double)device_rate * 1000.0;
	result.pad_ms = (double)backend->CurrentPaddingFrames() / (double)device_rate * 1000.0;
	result.stream_ms = backend->StreamLatencyMs();
	return result;
}

// level_ema と同じ "n/a" 表示規約(FormatLevelEma() 参照)。valid==false のときは残留値(0.00 等)を
// 出さず "n/a" にする(レビュー指摘 Medium-5)。
void FormatMsOrNa(bool valid, double value_ms, char *out, size_t out_size)
{
	if (!valid)
		snprintf(out, out_size, "n/a");
	else
		snprintf(out, out_size, "%.2f", value_ms);
}

} // namespace

namespace diagnostics {

void LogStartDump(const StartDumpInfo &info)
{
	// レビュー指摘 M-7: log_level 設定に関わらず常に出す(obs_log_always()、AC-22 の証跡保護)。
	obs_log_always(LOG_INFO,
		       "start dump: plugin_version=%s obs_version=%s backend=%s device_id=%s device_name=\"%s\" "
		       "resolved_by=%s "
		       "device_format tag=%u channels=%u samples_per_sec=%u bits_per_sample=%u is_float=%d "
		       "stream_format tag=%u channels=%u samples_per_sec=%u bits_per_sample=%u is_float=%d "
		       "obs_sample_rate=%u conversion_sample_rate=%u buffer_chunks=%u(requested %u) autoconvert=%d",
		       PLUGIN_VERSION, obs_get_version_string(), info.backend_name, info.device_id.c_str(),
		       info.device_name.c_str(), info.resolved_by.c_str(), info.device_format_tag, info.device_channels,
		       info.device_sample_rate, info.device_bits_per_sample, info.device_is_float ? 1 : 0,
		       info.stream_format_tag, info.stream_channels, info.stream_sample_rate,
		       info.stream_bits_per_sample, info.stream_is_float ? 1 : 0, info.obs_sample_rate,
		       info.conversion_sample_rate, info.buffer_chunks, info.buffer_chunks_requested,
		       info.autoconvert_pcm ? 1 : 0);
}

void LogDiagnosticsSnapshot(const DiagnosticsSnapshot &snapshot)
{
	const std::string text = BuildDiagnosticsText(snapshot);
	// レビュー指摘 M-7: log_level 設定に関わらず常に出す(obs_log_always()、AC-22 の証跡保護)。
	// Slice 9: AC-22 の 15 項目の後ろに output_mode/互換モードのイベントカウンタを追記している
	// (BuildDiagnosticsText() 参照。AC-22 の 15 項目自体の件数・順序は変わらない)。
	obs_log_always(LOG_INFO, "diagnostics snapshot (AC-22, 15 items + compat info):\n%s", text.c_str());
}

std::string GetOsVersionString()
{
	const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!ntdll)
		return "unknown";

	auto *rtl_get_version = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
	if (!rtl_get_version)
		return "unknown";

	RTL_OSVERSIONINFOW info{};
	info.dwOSVersionInfoSize = sizeof(info);
	if (rtl_get_version(&info) != 0 /* STATUS_SUCCESS */)
		return "unknown";

	// dwMajorVersion==10 は Windows 10/11 共通。ビルド番号で表示上のみ区別する(実体は同じ
	// メジャー/マイナー値。レビュー等で紛らわしくないよう "Windows 11" 側にも build を残す)。
	const char *product_name = "Windows";
	if (info.dwMajorVersion == 10 && info.dwMinorVersion == 0)
		product_name = info.dwBuildNumber >= kWindows11MinBuildNumber ? "Windows 11" : "Windows 10";

	char buf[96];
	snprintf(buf, sizeof(buf), "%s (%lu.%lu.%lu)", product_name, (unsigned long)info.dwMajorVersion,
		 (unsigned long)info.dwMinorVersion, (unsigned long)info.dwBuildNumber);
	return buf;
}

} // namespace diagnostics

DiagnosticsLogger::~DiagnosticsLogger()
{
	StopThread();
	LogFinalSummary();
}

void DiagnosticsLogger::Start(AudioMixSource *mix_source, RingBuffer *ring_buffer, WasapiOutput *backend,
			      const std::atomic<float> *cpu_control_pct)
{
	if (running_)
		return;

	mix_source_ = mix_source;
	ring_buffer_ = ring_buffer;
	backend_ = backend;
	cpu_control_pct_ = cpu_control_pct;
	stop_requested_.store(false, std::memory_order_relaxed);
	running_ = true;

	thread_ = std::thread(&DiagnosticsLogger::ThreadMain, this);
}

void DiagnosticsLogger::StopThread()
{
	if (!running_)
		return;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_requested_.store(true, std::memory_order_relaxed);
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();

	running_ = false;
}

void DiagnosticsLogger::LogFinalSummary()
{
	if (!mix_source_ || !ring_buffer_ || !backend_)
		return;

	// StopThread() で周期ログスレッドは既に止まっている。ここは呼び出し元
	// (OutputInstance::Stop())が AudioMixSource.Stop()/WasapiOutput.Stop() を呼んだ後に呼ぶため、
	// カウンタは最終値になっている(レビュー指摘 #8)。
	LogSummaryLine();

	mix_source_ = nullptr;
	ring_buffer_ = nullptr;
	backend_ = nullptr;
	cpu_control_pct_ = nullptr;
}

void DiagnosticsLogger::LogSummaryLine()
{
	uint64_t callbacks = 0, frames = 0;
	uint32_t last_frames = 0, min_frames = 0, max_frames = 0, ts_discont = 0, ts_discont_neg = 0;
	mix_source_->GetCumulativeStats(&callbacks, &frames, &last_frames, &min_frames, &max_frames, &ts_discont,
					&ts_discont_neg);

	char level_ema_str[32];
	FormatLevelEma(backend_->DriftLevelEma(), level_ema_str, sizeof(level_ema_str));

	// Slice 8(AC-11/AC-12): 停止時点で分かる最後の値を出す。render/control の CPU% は直近の周期
	// サンプル(最大 2 秒前)のまま(render thread は既に停止済みのため SampleCpuUsage() を呼んでも
	// 更新されない。停止直前の値を「最終値」として扱う)。
	const float cpu_render_pct = backend_->RenderCpuPercent();
	const float cpu_control_pct = cpu_control_pct_ ? cpu_control_pct_->load(std::memory_order_relaxed) : 0.0f;
	// レビュー指摘 Medium-5: 停止直前の最終サンプル(RenderLoop が停止する前に atomic store した
	// 最後の値)をそのまま出す。valid==false(Open 未完了のまま停止した等)なら "n/a" にし、
	// 残留値(0.00 等)を出さない。
	const EstLatencyBreakdown est_latency = ComputeEstLatencyBreakdown(backend_);
	char est_latency_str[32];
	char lat_ring_str[32];
	char lat_pad_str[32];
	char lat_stream_str[32];
	FormatMsOrNa(est_latency.valid, est_latency.TotalMs(), est_latency_str, sizeof(est_latency_str));
	FormatMsOrNa(est_latency.valid, est_latency.ring_ms, lat_ring_str, sizeof(lat_ring_str));
	FormatMsOrNa(est_latency.valid, est_latency.pad_ms, lat_pad_str, sizeof(lat_pad_str));
	FormatMsOrNa(est_latency.valid, est_latency.stream_ms, lat_stream_str, sizeof(lat_stream_str));
	const double cb_max_us = (double)mix_source_->CallbackMaxNs() / 1000.0;

	// レビュー指摘 M-7: 停止サマリは log_level 設定に関わらず常に出す(obs_log_always()、
	// AC-22 の証跡保護)。周期ログ(ThreadMain 側の "mix stats")は従来どおり obs_log() の閾値に従う。
	// Slice 6(S3 持ち越し、ブリーフ実装内容 13): drift_ins/drop は定常モードの補正のみ、
	// drift_recovery は高速復帰モード中の補正を別カウントで出す(AC-22 の DiagnosticsSnapshot/
	// BuildDiagnosticsText の固定 15 項目は変更しない。この行はそれとは別の自由形式の診断ログ)。
	// レビュー指摘 Medium-6: est_latency_ms は lat_ring_ms/lat_pad_ms/lat_stream_ms の単純合算
	// (上界推定。二重計上の可能性はコメント/README 参照)。ログ行はすべて key=value の単純トークンで
	// 出す(measure-report.py の _KV_RE がスペース区切りで value をパースするため、value に "(" 等の
	// 追加テキストを混ぜない)。
	obs_log_always(
		LOG_INFO,
		"stop summary: track=%d mix_index=%zu callbacks_total=%llu frames_total=%llu "
		"min/max_frames_total=%u/%u ts_discont_total=%u neg=%u ring_overrun=%llu ring_oversize=%llu "
		"wasapi_underrun=%llu clipped_samples=%llu drift_ins=%llu drift_drop=%llu drift_recovery=%llu "
		"level_ema_frames=%s lat_ring_ms=%s lat_pad_ms=%s lat_stream_ms=%s est_latency_ms=%s cb_max_us=%.1f "
		"cpu_render_pct=%.2f cpu_control_pct=%.2f ring_peak=%u",
		mix_source_->AudioTrack(), mix_source_->MixIndex(), (unsigned long long)callbacks,
		(unsigned long long)frames, min_frames, max_frames, ts_discont, ts_discont_neg,
		(unsigned long long)ring_buffer_->OverrunCount(), (unsigned long long)ring_buffer_->OversizeCount(),
		(unsigned long long)backend_->UnderrunCount(), (unsigned long long)backend_->ClipCount(),
		(unsigned long long)backend_->DriftInsertedFrames(), (unsigned long long)backend_->DriftDroppedFrames(),
		(unsigned long long)backend_->DriftRecoveryFrames(), level_ema_str, lat_ring_str, lat_pad_str,
		lat_stream_str, est_latency_str, cb_max_us, (double)cpu_render_pct, (double)cpu_control_pct,
		ring_buffer_->PeakLevel());
}

void DiagnosticsLogger::ThreadMain()
{
	// 初回 tick(開始後 2 秒)で callback が 1 件も届いていなければ 1 回だけ警告する
	// (S1 の LogThreadMain から移設。継続監視・再登録のウォッチドッグ本体は S6)。
	bool first_tick = true;

	std::unique_lock<std::mutex> lock(mutex_);
	while (!stop_requested_.load(std::memory_order_relaxed)) {
		cv_.wait_for(lock, kLogInterval, [this] { return stop_requested_.load(std::memory_order_relaxed); });
		if (stop_requested_.load(std::memory_order_relaxed))
			break;

		uint64_t callbacks = 0, frames = 0;
		uint32_t last_frames = 0, min_frames = 0, max_frames = 0, ts_discont = 0, ts_discont_neg = 0;
		mix_source_->GetCumulativeStats(&callbacks, &frames, &last_frames, &min_frames, &max_frames,
						&ts_discont, &ts_discont_neg);

		if (first_tick) {
			if (callbacks == 0) {
				obs_log(LOG_WARNING,
					"no raw audio callback received within 2s after start (track=%d "
					"mix_index=%zu); check the audio_track setting and OBS audio mixes",
					mix_source_->AudioTrack(), mix_source_->MixIndex());
			}
			first_tick = false;
		}

		float peak_l = 0.0f, peak_r = 0.0f, rms_linear = 0.0f;
		mix_source_->ConsumeWindowedLevels(&peak_l, &peak_r, &rms_linear);

		uint64_t ring_overrun = ring_buffer_->OverrunCount();
		uint64_t ring_oversize = ring_buffer_->OversizeCount();
		uint32_t ring_level = ring_buffer_->Level();
		uint64_t underrun = backend_->UnderrunCount();
		uint64_t clipped_samples = backend_->ClipCount();
		uint64_t drift_ins = backend_->DriftInsertedFrames();
		uint64_t drift_drop = backend_->DriftDroppedFrames();
		uint64_t drift_recovery = backend_->DriftRecoveryFrames();
		char level_ema_str[32];
		FormatLevelEma(backend_->DriftLevelEma(), level_ema_str, sizeof(level_ema_str));

		// Slice 8(AC-11/AC-12): この 2 秒 tick で render thread の CPU% を 1 回だけサンプルする
		// (rt-audio.md: GetThreadTimes は RT ループ外のこの周期スレッドで呼ぶ)。
		backend_->SampleCpuUsage();
		const float cpu_render_pct = backend_->RenderCpuPercent();
		const float cpu_control_pct = cpu_control_pct_ ? cpu_control_pct_->load(std::memory_order_relaxed)
							       : 0.0f;
		// レビュー指摘 Medium-3/Medium-6: RenderLoop が計算済みの値をそのまま使い、内訳
		// (lat_ring_ms/lat_pad_ms/lat_stream_ms)も個別フィールドで出す(est_latency_ms はその単純合算=
		// 上界推定。README/コメント参照)。
		const EstLatencyBreakdown est_latency = ComputeEstLatencyBreakdown(backend_);
		char est_latency_str[32];
		char lat_ring_str[32];
		char lat_pad_str[32];
		char lat_stream_str[32];
		FormatMsOrNa(est_latency.valid, est_latency.TotalMs(), est_latency_str, sizeof(est_latency_str));
		FormatMsOrNa(est_latency.valid, est_latency.ring_ms, lat_ring_str, sizeof(lat_ring_str));
		FormatMsOrNa(est_latency.valid, est_latency.pad_ms, lat_pad_str, sizeof(lat_pad_str));
		FormatMsOrNa(est_latency.valid, est_latency.stream_ms, lat_stream_str, sizeof(lat_stream_str));
		const double cb_max_us = (double)mix_source_->CallbackMaxNs() / 1000.0;
		// レビュー指摘 Medium-4: AC-12 の cb_max 判定は 2 秒窓 max の分布を主とする(cb_max_us は
		// セッション累積の参考値のまま残す)。
		const double cb_max_2s_us = (double)mix_source_->ConsumeCallbackMax2sNs() / 1000.0;
		const uint32_t ring_peak = ring_buffer_->PeakLevel();

		if (backend_->HasFailed()) {
			// Slice 6: 状態機械の遷移(Running→Recovering/Failed)は OutputInstance の control
			// thread が backend_->HasFailed() を自前でポーリングして行う(RunPeriodicTasks())。
			// この診断スレッドは状態遷移を行わず、警告ログのみを出す(診断スレッドから
			// OutputInstance::Stop() 相当を呼ぶと自スレッド join でデッドロックするため。旧レビュー
			// 指摘 #4 と同じ理由)。control thread が CleanupFailedAttempt() でこのスレッド自体を
			// 止めるまでの間、最大 kLogInterval(2s)おきに繰り返し出ることがある。
			obs_log(LOG_WARNING,
				"wasapi backend reported a failure; output may be silent until reconnected");
		}

		// 2026-09-01 配布準備改定: 既定 log_level=info の配布環境で 2 秒周期ログがログを肥大させない
		// よう LOG_DEBUG へ落とす(開始ダンプ・停止サマリ等のイベントログは obs_log_always() で info
		// のまま)。
		obs_log(LOG_DEBUG,
			"mix stats: track=%d mix_index=%zu callbacks_total=%llu frames_total=%llu last_frames=%u "
			"min/max_frames_total=%u/%u peak2s L/R=%.1f/%.1f dBFS rms2s=%.1f dBFS ts_discont_total=%u "
			"neg=%u ring_level=%u ring_overrun=%llu ring_oversize=%llu wasapi_underrun=%llu "
			"clipped_samples=%llu drift_ins=%llu drift_drop=%llu drift_recovery=%llu level_ema_frames=%s "
			"lat_ring_ms=%s lat_pad_ms=%s lat_stream_ms=%s est_latency_ms=%s cb_max_us=%.1f "
			"cb_max2s_us=%.1f cpu_render_pct=%.2f cpu_control_pct=%.2f ring_peak=%u",
			mix_source_->AudioTrack(), mix_source_->MixIndex(), (unsigned long long)callbacks,
			(unsigned long long)frames, last_frames, min_frames, max_frames, LinearToDbfs(peak_l),
			LinearToDbfs(peak_r), LinearToDbfs(rms_linear), ts_discont, ts_discont_neg, ring_level,
			(unsigned long long)ring_overrun, (unsigned long long)ring_oversize,
			(unsigned long long)underrun, (unsigned long long)clipped_samples,
			(unsigned long long)drift_ins, (unsigned long long)drift_drop,
			(unsigned long long)drift_recovery, level_ema_str, lat_ring_str, lat_pad_str, lat_stream_str,
			est_latency_str, cb_max_us, cb_max_2s_us, (double)cpu_render_pct, (double)cpu_control_pct,
			ring_peak);
	}
}
