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

// 対応 REQ: REQ-AUDIO-001, REQ-AUDIO-002, REQ-AUDIO-006 / Slice 1, Slice 2

#include "audio-mix-source.hpp"
#include "ring-buffer.hpp"
#include "track-map.hpp"

#include <obs-module.h>
#include <obs.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <cmath>
#include <cstdlib>

namespace {

// RT スレッド(audio-io thread)から呼ばれる想定の lock-free float 加算。
// std::atomic<float>::fetch_add は C++20 以降のみのため CAS ループで代替する。
// ロック・ヒープ確保・システムコールを伴わないため RT 安全。
void AtomicAddFloat(std::atomic<float> &target, float delta)
{
	float expected = target.load(std::memory_order_relaxed);
	while (!target.compare_exchange_weak(expected, expected + delta, std::memory_order_relaxed)) {
		// expected は失敗時に最新値へ更新される。
	}
}

void AtomicMaxUint32(std::atomic<uint32_t> &target, uint32_t candidate)
{
	uint32_t current = target.load(std::memory_order_relaxed);
	while (candidate > current && !target.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
	}
}

void AtomicMinUint32(std::atomic<uint32_t> &target, uint32_t candidate)
{
	uint32_t current = target.load(std::memory_order_relaxed);
	while (candidate < current && !target.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
	}
}

void AtomicMaxFloat(std::atomic<float> &target, float candidate)
{
	float current = target.load(std::memory_order_relaxed);
	while (candidate > current && !target.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
	}
}

// Slice 8(AC-12): cb_max_ns_ 用。AtomicMaxUint32 と同じ CAS ループを uint64_t へ。
void AtomicMaxUint64(std::atomic<uint64_t> &target, uint64_t candidate)
{
	uint64_t current = target.load(std::memory_order_relaxed);
	while (candidate > current && !target.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
	}
}

} // namespace

AudioMixSource::~AudioMixSource()
{
	Stop();
}

void AudioMixSource::Start(int audio_track, uint32_t samples_per_sec)
{
	// Start()/Stop() を直列化する(state_mutex_)。二重 Start や Start/Stop の競合を防ぐ(S1 findings #1)。
	std::lock_guard<std::mutex> state_lock(state_mutex_);

	if (running_.load(std::memory_order_acquire))
		return;

	bool clamped = false;
	mix_index_ = audio_track_map::ToMixIndex(audio_track, &clamped);
	audio_track_ = audio_track_map::ClampAudioTrack(audio_track, nullptr);
	if (clamped) {
		obs_log(LOG_WARNING, "audio_track %d is out of range (1-%d); clamped to %d", audio_track,
			audio_track_map::kMaxAudioTrack, audio_track_);
	}

	// 統計をリセット(前回 Start/Stop の残骸を引きずらない)。
	callback_count_.store(0, std::memory_order_relaxed);
	total_frames_.store(0, std::memory_order_relaxed);
	last_frames_.store(0, std::memory_order_relaxed);
	min_frames_.store(UINT32_MAX, std::memory_order_relaxed);
	max_frames_.store(0, std::memory_order_relaxed);
	peak_l_.store(0.0f, std::memory_order_relaxed);
	peak_r_.store(0.0f, std::memory_order_relaxed);
	sumsq_l_.store(0.0f, std::memory_order_relaxed);
	sumsq_r_.store(0.0f, std::memory_order_relaxed);
	rms_sample_count_.store(0, std::memory_order_relaxed);
	ts_discontinuity_count_.store(0, std::memory_order_relaxed);
	ts_discontinuity_neg_count_.store(0, std::memory_order_relaxed);
	cb_max_ns_.store(0, std::memory_order_relaxed);
	cb_max_2s_ns_.store(0, std::memory_order_relaxed);
	rt_has_prev_timestamp_ = false;
	rt_prev_timestamp_ = 0;
	rt_prev_frames_ = 0;

	// S2: 変換先レートは OutputInstance が WASAPI デバイス開通後に解決して渡す
	// (ブリーフ実装内容 10。obs_get_audio_info の OBS 内部レートではなくデバイスのミックスレート)。
	rt_samples_per_sec_ = samples_per_sec;

	// audio_convert_info は全フィールドを明示する(規約。0 値は既定値補完されるが依存しない)。
	audio_convert_info conv = {};
	conv.format = AUDIO_FORMAT_FLOAT;
	conv.samples_per_sec = rt_samples_per_sec_;
	conv.speakers = SPEAKERS_STEREO;
	conv.allow_clipping = false;

	running_.store(true, std::memory_order_release);

	obs_add_raw_audio_callback(mix_index_, &conv, &AudioMixSource::AudioCallback, this);

	obs_log(LOG_INFO, "AudioMixSource started: audio_track=%d mix_index=%zu samples_per_sec=%u", audio_track_,
		mix_index_, rt_samples_per_sec_);
}

void AudioMixSource::Stop()
{
	std::lock_guard<std::mutex> state_lock(state_mutex_);

	if (!running_.load(std::memory_order_acquire))
		return;

	// 破棄順序: raw callback 解除 → running_ を false へ(rt-audio.md)。
	obs_remove_raw_audio_callback(mix_index_, &AudioMixSource::AudioCallback, this);

	running_.store(false, std::memory_order_release);

	// callback 解除後に sink_ をクリアする(回帰レビュー指摘 #2)。obs_remove_raw_audio_callback
	// によりこの時点で HandleAudioData の呼び出しは発生しないはずだが、呼び出し元
	// (OutputInstance)がこの直後に ring_buffer_ を再構築/破棄することを考えると、sink_ に
	// 破棄済み RingBuffer への生ポインタを残さないことが安全側の防御になる。
	sink_.store(nullptr, std::memory_order_release);

	obs_log(LOG_INFO, "AudioMixSource stopped: audio_track=%d mix_index=%zu", audio_track_, mix_index_);
}

void AudioMixSource::Reregister(uint32_t samples_per_sec)
{
	// Start()/Stop() と直列化する(Reregister() 実行中に Stop() が割り込んで mix_index_ 等を
	// 変更しないようにするため)。control thread からのみ呼ぶ想定(RT ではない)。
	std::lock_guard<std::mutex> state_lock(state_mutex_);

	if (!running_.load(std::memory_order_acquire))
		return;

	// Stop()+Start() と異なり累積統計(callback_count_/total_frames_/peak_*_/sumsq_*_ 等)は
	// 意図的にリセットしない(D-06 追従の目的は「obs_reset_audio 後の内部構成に合わせて raw
	// callback を貼り直す」ことであり、診断上の連続性を保ちたいため。ブリーフ: 「統計継続」)。
	obs_remove_raw_audio_callback(mix_index_, &AudioMixSource::AudioCallback, this);

	rt_samples_per_sec_ = samples_per_sec;
	// 再登録の前後でコールバックの timestamp が連続している保証はない(obs_reset_audio 自体が
	// パイプラインを作り直すため)。誤って ts_discontinuity としてカウントしないよう基準をクリアする。
	rt_has_prev_timestamp_ = false;
	rt_prev_timestamp_ = 0;
	rt_prev_frames_ = 0;

	audio_convert_info conv = {};
	conv.format = AUDIO_FORMAT_FLOAT;
	conv.samples_per_sec = rt_samples_per_sec_;
	conv.speakers = SPEAKERS_STEREO;
	conv.allow_clipping = false;

	obs_add_raw_audio_callback(mix_index_, &conv, &AudioMixSource::AudioCallback, this);

	obs_log(LOG_INFO,
		"AudioMixSource reregistered (obs_reset_audio follow-up): audio_track=%d mix_index=%zu "
		"samples_per_sec=%u",
		audio_track_, mix_index_, rt_samples_per_sec_);
}

void AudioMixSource::AudioCallback(void *param, size_t mix_idx, audio_data *data)
{
	UNUSED_PARAMETER(mix_idx);
	auto *self = static_cast<AudioMixSource *>(param);
	self->HandleAudioData(data);
}

void AudioMixSource::HandleAudioData(audio_data *data)
{
	// RT スレッド("audio-io: audio thread", input_mutex 保持中): ヒープ確保・ロック・blog・COM・
	// ファイル I/O・raw callback add/remove は禁止(rt-audio.md)。atomic 更新・RingBuffer::TryPush
	// (ヒープ確保なし)のみ許可。
	if (!data || !data->data[0] || data->frames == 0)
		return;

	// Slice 8(AC-12、cb_max <1ms 判定): os_gettime_ns() は QPC ベース(RT 安全、
	// .deps/obs-studio-31.1.1/libobs/util/platform-windows.c で確認済み)なので前後読み+atomic 更新
	// のみで RT 規約に収まる。処理本体全体(push・統計・不連続検出)を計測対象にする。
	const uint64_t cb_start_ns = os_gettime_ns();

	callback_count_.fetch_add(1, std::memory_order_relaxed);
	total_frames_.fetch_add(data->frames, std::memory_order_relaxed);
	last_frames_.store(data->frames, std::memory_order_relaxed);
	AtomicMinUint32(min_frames_, data->frames);
	AtomicMaxUint32(max_frames_, data->frames);

	// AUDIO_FORMAT_FLOAT は interleaved: data->data[0] に L,R,L,R...(旧#10)。
	const auto *samples = reinterpret_cast<const float *>(data->data[0]);

	RingBuffer *sink = sink_.load(std::memory_order_acquire);
	if (sink)
		sink->TryPush(samples, data->frames); // 満杯なら内部で discard + overrun カウンタ++(RT 安全)。

	float peak_l = 0.0f;
	float peak_r = 0.0f;
	float sumsq_l = 0.0f;
	float sumsq_r = 0.0f;
	for (uint32_t i = 0; i < data->frames; ++i) {
		float l = samples[i * 2];
		float r = samples[i * 2 + 1];
		float abs_l = l < 0.0f ? -l : l;
		float abs_r = r < 0.0f ? -r : r;
		if (abs_l > peak_l)
			peak_l = abs_l;
		if (abs_r > peak_r)
			peak_r = abs_r;
		sumsq_l += l * l;
		sumsq_r += r * r;
	}

	AtomicMaxFloat(peak_l_, peak_l);
	AtomicMaxFloat(peak_r_, peak_r);
	AtomicAddFloat(sumsq_l_, sumsq_l);
	AtomicAddFloat(sumsq_r_, sumsq_r);
	rms_sample_count_.fetch_add(data->frames, std::memory_order_relaxed);

	// timestamp 不連続検出: このコールバックは常に同一スレッドから逐次呼ばれるため
	// rt_prev_timestamp_ 等は atomic 不要(このインスタンス内のみで読み書き)。
	// 期待デルタは「前回チャンクの frames」が表す時間長で計算する(今回の frames ではない。
	// 今回 timestamp - 前回 timestamp は前回チャンクの再生時間に等しいはず)。
	// 差分は符号付きで取り、逆行(負)と過大ジャンプを区別してから判定する。
	if (rt_has_prev_timestamp_ && rt_samples_per_sec_ > 0 && rt_prev_frames_ > 0) {
		double expected_delta_ns = (double)rt_prev_frames_ * 1000000000.0 / (double)rt_samples_per_sec_;
		int64_t actual_delta_ns = (int64_t)(data->timestamp - rt_prev_timestamp_);
		double tolerance_ns = expected_delta_ns * 0.5;
		bool is_negative = actual_delta_ns < 0;
		bool is_large_jump = !is_negative &&
				     std::fabs((double)actual_delta_ns - expected_delta_ns) > tolerance_ns;
		if (is_negative || is_large_jump) {
			ts_discontinuity_count_.fetch_add(1, std::memory_order_relaxed);
			if (is_negative)
				ts_discontinuity_neg_count_.fetch_add(1, std::memory_order_relaxed);
		}
	}
	rt_prev_timestamp_ = data->timestamp;
	rt_prev_frames_ = data->frames;
	rt_has_prev_timestamp_ = true;

	// Slice 8 レビュー Medium-4: セッション累積(cb_max_ns_、参考値)と直近 2 秒窓(cb_max_2s_ns_、
	// AC-12 の主判定に使う)の両方を同じ計測値で更新する。
	const uint64_t cb_elapsed_ns = os_gettime_ns() - cb_start_ns;
	AtomicMaxUint64(cb_max_ns_, cb_elapsed_ns);
	AtomicMaxUint64(cb_max_2s_ns_, cb_elapsed_ns);
}

void AudioMixSource::GetCumulativeStats(uint64_t *callbacks, uint64_t *frames, uint32_t *last_frames,
					uint32_t *min_frames, uint32_t *max_frames, uint32_t *ts_discontinuity,
					uint32_t *ts_discontinuity_neg) const
{
	if (callbacks)
		*callbacks = callback_count_.load(std::memory_order_relaxed);
	if (frames)
		*frames = total_frames_.load(std::memory_order_relaxed);
	if (last_frames)
		*last_frames = last_frames_.load(std::memory_order_relaxed);
	if (min_frames) {
		uint32_t min_val = min_frames_.load(std::memory_order_relaxed);
		*min_frames = (min_val == UINT32_MAX) ? 0 : min_val;
	}
	if (max_frames)
		*max_frames = max_frames_.load(std::memory_order_relaxed);
	if (ts_discontinuity)
		*ts_discontinuity = ts_discontinuity_count_.load(std::memory_order_relaxed);
	if (ts_discontinuity_neg)
		*ts_discontinuity_neg = ts_discontinuity_neg_count_.load(std::memory_order_relaxed);
}

void AudioMixSource::ConsumeWindowedLevels(float *peak_l_linear, float *peak_r_linear, float *rms_linear)
{
	// 直近区間(呼び出し元の周期)のピーク/RMS として exchange でリセットする。
	// 呼び出し元は単一スレッドであること(複数スレッドが呼ぶと exchange によるリセットが競合する)。
	float peak_l = peak_l_.exchange(0.0f, std::memory_order_relaxed);
	float peak_r = peak_r_.exchange(0.0f, std::memory_order_relaxed);
	float sumsq_l = sumsq_l_.exchange(0.0f, std::memory_order_relaxed);
	float sumsq_r = sumsq_r_.exchange(0.0f, std::memory_order_relaxed);
	uint64_t rms_count = rms_sample_count_.exchange(0, std::memory_order_relaxed);

	if (peak_l_linear)
		*peak_l_linear = peak_l;
	if (peak_r_linear)
		*peak_r_linear = peak_r;
	if (rms_linear)
		*rms_linear = (rms_count > 0) ? std::sqrt((sumsq_l + sumsq_r) / (2.0f * (float)rms_count)) : 0.0f;
}

// レビュー指摘 L-11: audio_track の config.json 読み込みは output-instance.cpp の LoadConfig() へ
// 統合した(旧: audio_mix_source::LoadAudioTrackFromConfig())。
