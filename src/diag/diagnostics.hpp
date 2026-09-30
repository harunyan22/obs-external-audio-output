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

// 対応 REQ: REQ-LOG-001, REQ-AUDIO-005, REQ-DEVICE-001, REQ-UI-008 / Slice 2, Slice 3, Slice 4, Slice 5
//
// 開始時ダンプ(D-18、REQ-LOG-001 の最小実装)と、AudioMixSource/RingBuffer/WasapiOutput の
// カウンタを集約する 2 秒周期ログスレッド。S1 で AudioMixSource が内蔵していたログスレッドの
// 役割をここへ移す(S1 の 2 秒周期は維持)。
// Slice 3: ドリフト補正の統計(drift_ins/drift_drop/level_ema)を周期ログ・停止サマリに追加。
// Slice 4: 開始時ダンプにデバイス解決結果(device_id/device_name/resolved_by)を追加。
// Slice 5: AC-22 の全 15 項目・Dock 連携を完全実装する。純粋な整形ロジックは diagnostics-text.hpp
// (DiagnosticsSnapshot/BuildDiagnosticsText、libobs 非依存)に分離し、ここでは obs_log 出力と
// Windows 依存の OS Version 取得(RtlGetVersion)のみを担う。

#pragma once

#include "diagnostics-text.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

class AudioMixSource;
class RingBuffer;
class WasapiOutput;

namespace diagnostics {

// Start() 成功時に 1 回だけ出す開始時ダンプの内容(ブリーフ実装内容 17)。
// device_* と stream_* は別物(再レビュー指摘 M): device_* は GetMixFormat() が返したデバイスの
// 生フォーマット(変換前)、stream_* は IAudioClient::Initialize に実際に渡したフォーマット
// (AUTOCONVERTPCM 使用時は素の float 等、device_* と異なり得る)。
struct StartDumpInfo {
	// Slice 4: device-matcher で解決した実際の endpoint id("default" 解決時は既定デバイスの実 id、
	// 取得できなければ "default" のまま)。短縮表示はしない(実運用でのコピペしやすさを優先)。
	std::string device_id;
	// 解決したデバイスの friendly name(Slice 4。取得できなければ空文字列)。
	std::string device_name;
	// 解決経路: "default" / "id" / "name"(D-14。device_matcher::ResolvedBy に対応)。
	std::string resolved_by;
	uint32_t device_sample_rate = 0;
	uint16_t device_channels = 0;
	uint16_t device_bits_per_sample = 0;
	uint16_t device_format_tag = 0;
	bool device_is_float = false;
	uint32_t stream_sample_rate = 0;
	uint16_t stream_channels = 0;
	uint16_t stream_bits_per_sample = 0;
	uint16_t stream_format_tag = 0;
	bool stream_is_float = false;
	uint32_t obs_sample_rate = 0;
	uint32_t conversion_sample_rate = 0;
	// RingBuffer の実容量(2 の冪へ切り上げ後)。
	uint32_t buffer_chunks = 0;
	// レビュー指摘 M-9: buffer_preset/buffer_chunks(Advanced 設定)から要求されたチャンク数
	// (切り上げ前)。diagnostics-text.hpp の DiagnosticsSnapshot::buffer_chunks_requested と対。
	uint32_t buffer_chunks_requested = 0;
	const char *backend_name = "wasapi";
	// 非 float mix format に対して AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM を使って Initialize したか
	// (Slice 3、ARCHITECTURE.md §5 手順 2.5)。
	bool autoconvert_pcm = false;
};

// obs_log_always(LOG_INFO, ...) で 1 行(または複数行)出力する。RT スレッドからは呼ばない
// (OutputInstance::Start() の成功/失敗パス、UI/frontend イベントスレッドから呼ぶ想定)。
// レビュー指摘 M-7: AC-22 の証跡保護のため、log_level 設定(obs_log() の閾値)に関わらず常に出す
// (obs_log() ではなく obs_log_always() を使う)。
void LogStartDump(const StartDumpInfo &info);

// AC-22 の 15 項目スナップショットを obs_log_always(LOG_INFO, ...) へ出力する(BuildDiagnosticsText()
// の結果をそのまま 1 回の複数行ログとして出す)。RT スレッドからは呼ばない。OutputInstance::Start()
// の成功/失敗パスと Stop() 時の両方で呼ぶことで、開始ダンプ・停止サマリのいずれかに全項目が出る
// (AC-22 の合格基準)。レビュー指摘 M-7: log_level 設定に関わらず常に出す(obs_log_always() を使う)。
void LogDiagnosticsSnapshot(const DiagnosticsSnapshot &snapshot);

// 現在の OS バージョン文字列を取得する(RtlGetVersion 経由。GetVersionEx とは異なりアプリ
// マニフェストの互換シムの影響を受けない)。Windows 専用。UI/frontend イベントスレッドから呼ぶ想定
// (ヒープ確保を伴うため RT では呼ばない)。取得に失敗した場合は "unknown" を返す。
std::string GetOsVersionString();

} // namespace diagnostics

// 2 秒周期でカウンタを集約ログ出力するスレッド。RT スレッドではない(AudioMixSource/RingBuffer/
// WasapiOutput の atomic を読み取るだけで、対象オブジェクトの所有権は持たない)。
class DiagnosticsLogger {
public:
	DiagnosticsLogger() = default;
	~DiagnosticsLogger();

	DiagnosticsLogger(const DiagnosticsLogger &) = delete;
	DiagnosticsLogger &operator=(const DiagnosticsLogger &) = delete;

	// OutputInstance::Start() の成功パスの最後で呼ぶ(state_mutex_ 配下、RT ではない)。開始済みなら no-op。
	// cpu_control_pct: OutputInstance が RunPeriodicTasks() で継続サンプリングしている control thread
	// の CPU%(Slice 8、AC-12)。DiagnosticsLogger 自身は書き込まず読むだけ。nullptr なら周期ログ/
	// 停止サマリの cpu_control_pct は 0.0 として出す。
	void Start(AudioMixSource *mix_source, RingBuffer *ring_buffer, WasapiOutput *backend,
		   const std::atomic<float> *cpu_control_pct);

	// 周期ログスレッドを止める(join のみ、サマリはまだ出さない)。開始済みでなければ no-op。
	// OutputInstance::Stop() は AudioMixSource.Stop()/WasapiOutput.Stop() より前にこれを呼ぶことで、
	// カウンタが動いている最中にスレッドを止める(レビュー指摘 #8)。
	void StopThread();

	// 最終値でサマリを 1 行出し、内部ポインタをクリアする。StopThread() の後、かつ
	// OutputInstance が AudioMixSource.Stop()/WasapiOutput.Stop() を呼んだ後に呼ぶこと
	// (レビュー指摘 #8: サマリの値を最終値にするため)。ポインタが未設定(Start() 未実行/
	// 既に呼び出し済み)なら no-op。レビュー指摘 M-7: このサマリ行は log_level 設定に関わらず常に出す
	// (obs_log_always() を使う。AC-22 の証跡保護)。
	void LogFinalSummary();

private:
	void ThreadMain();
	void LogSummaryLine();

	AudioMixSource *mix_source_ = nullptr;
	RingBuffer *ring_buffer_ = nullptr;
	WasapiOutput *backend_ = nullptr;
	// Slice 8(AC-12): OutputInstance 所有の control thread CPU% atomic への非所有ポインタ。
	const std::atomic<float> *cpu_control_pct_ = nullptr;

	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable cv_;
	std::atomic<bool> stop_requested_{false};
	bool running_ = false; // 周期ログスレッドが動作中か
};
