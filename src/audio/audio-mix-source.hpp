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

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

struct audio_data;
class RingBuffer;

// libobs raw audio callback(mix_index)からトラックの音声データを受け取り、
// RT スレッド安全な統計(callback 回数・frames・ピーク・RMS・タイムスタンプ不連続)を蓄積しつつ、
// RingBuffer(SetSink で受け取る)へ push する(Slice 2)。統計の周期ログは Diagnostics(S2 で新設)が
// GetCumulativeStats/ConsumeWindowedLevels 経由で読み取って行う(S1 の内蔵ログスレッドは廃止)。
//
// mix_index = audio_track - 1 の変換は Start() 内(audio_track_map 経由)の 1 箇所のみで行う(D-01)。
class AudioMixSource {
public:
	AudioMixSource() = default;
	~AudioMixSource();

	AudioMixSource(const AudioMixSource &) = delete;
	AudioMixSource &operator=(const AudioMixSource &) = delete;

	// push 先の RingBuffer を設定する。Start() 前に呼ぶこと(非 RT。RT 中は HandleAudioData が
	// 生ポインタを読むのみ)。nullptr を渡すと push を行わない(S1 相当の統計のみモードに戻せる)。
	void SetSink(RingBuffer *sink) { sink_.store(sink, std::memory_order_release); }

	// audio_track(1〜6、範囲外はクランプ+警告ログ)と、変換先サンプルレート(WASAPI デバイスの
	// ミックスレート。OutputInstance がデバイス開通後に解決して渡す、ブリーフ実装内容 10)を指定して
	// raw audio callback を開始する。開始済みなら no-op。
	void Start(int audio_track, uint32_t samples_per_sec);

	// raw audio callback を解除する。停止済みなら no-op。解除後に SetSink(nullptr) を行う
	// (回帰レビュー指摘 #2: 呼び出し元が直後に sink の RingBuffer を再構築/破棄する経路への防御)。
	void Stop();

	// Slice 6(D-06、obs_reset_audio 追従、ブリーフ実装内容 10): raw callback を remove→add で
	// 貼り直す(Stop()+Start() と異なり累積統計をリセットしない。「統計継続」)。audio_track/
	// mix_index は変更しない(変更したい場合は Stop()+Start() を使うこと)。samples_per_sec は
	// 変換先レート(通常は WASAPI デバイスのミックスレートで obs_reset_audio の前後で不変。
	// OutputInstance の control thread が保持している最終値をそのまま渡す想定)。未開始中
	// (!IsRunning())は no-op。control thread から呼ぶ(RT ではない)。
	void Reregister(uint32_t samples_per_sec);

	bool IsRunning() const { return running_.load(std::memory_order_acquire); }

	int AudioTrack() const { return audio_track_; }
	size_t MixIndex() const { return mix_index_; }

	// 非リセットの累積統計を取得する(RT ではない任意のスレッドから安全。Diagnostics が周期的に呼ぶ)。
	// min_frames は一度も更新されていない場合 0 を返す(S1 の UINT32_MAX センチネルはここで正規化)。
	void GetCumulativeStats(uint64_t *callbacks, uint64_t *frames, uint32_t *last_frames, uint32_t *min_frames,
				uint32_t *max_frames, uint32_t *ts_discontinuity, uint32_t *ts_discontinuity_neg) const;

	// 直近区間のピーク/RMS(線形値)を取得し、内部カウンタをリセットする(exchange)。
	// 呼び出し元は単一スレッドであること(S2 では Diagnostics の周期ログスレッドのみが呼ぶ想定)。
	void ConsumeWindowedLevels(float *peak_l_linear, float *peak_r_linear, float *rms_linear);

	// Slice 8(AC-12): callback 1 回あたりの処理時間(HandleAudioData の os_gettime_ns 前後差、ns)の
	// セッション内累積最大値。Start() でリセットする(min/max_frames と同じ「セッション累積」の
	// 扱いで、2 秒窓でリセットしない)。レビュー指摘 Medium-4: AC-12 の cb_max 判定は
	// ConsumeCallbackMax2sNs() の 2 秒窓 max の分布を主とし、この値は参考値(セッション全体の
	// ワーストケース 1 点)として扱う。
	uint64_t CallbackMaxNs() const { return cb_max_ns_.load(std::memory_order_relaxed); }

	// Slice 8 レビュー Medium-4: CallbackMaxNs() と並行して更新される「直近 2 秒窓」の最大値。
	// Diagnostics の周期ログスレッド(2 秒ごと)が呼ぶたびに現在値を返して 0 へリセットする
	// (ConsumeWindowedLevels() と同じ exchange パターン)。呼び出し元は単一スレッドであること。
	uint64_t ConsumeCallbackMax2sNs() { return cb_max_2s_ns_.exchange(0, std::memory_order_relaxed); }

private:
	static void AudioCallback(void *param, size_t mix_idx, audio_data *data);
	// RT スレッド("audio-io: audio thread")から呼ばれる。ヒープ確保・ロック・blog・COM・I/O 禁止。
	void HandleAudioData(audio_data *data);

	std::atomic<bool> running_{false};
	int audio_track_ = 0;
	size_t mix_index_ = 0;

	// Start()/Stop() 全体を直列化する(UI/frontend イベントスレッド、または OutputInstance の
	// state_mutex_ 配下から呼ばれる想定。RT では使わない)。
	std::mutex state_mutex_;

	// RingBuffer への push 先。SetSink() は非 RT、HandleAudioData からは load(acquire) のみ(RT 安全)。
	std::atomic<RingBuffer *> sink_{nullptr};

	// RT コールバックは常に同一スレッド("audio-io: audio thread")から逐次呼ばれるため、
	// callback 呼び出しをまたぐ内部状態(このインスタンスのみが読み書き)は atomic 不要。
	uint32_t rt_samples_per_sec_ = 48000;
	uint64_t rt_prev_timestamp_ = 0;
	uint32_t rt_prev_frames_ = 0;
	bool rt_has_prev_timestamp_ = false;

	// --- Diagnostics(非 RT)から読む統計。RT スレッドからは atomic 更新のみ ---
	std::atomic<uint64_t> callback_count_{0};
	std::atomic<uint64_t> total_frames_{0};
	std::atomic<uint32_t> last_frames_{0};
	std::atomic<uint32_t> min_frames_{UINT32_MAX};
	std::atomic<uint32_t> max_frames_{0};
	// peak_l_/peak_r_/sumsq_*_ の読み手は ConsumeWindowedLevels の呼び出し元 1 つだけ(exchange でリセット)。
	// 将来レベルメーター(S5、QTimer 30Hz 読み取り)を追加するときは、この atomic を共用せず
	// 別の atomic<float> を新設すること(読み手が増えると exchange によるリセットが競合する)。
	std::atomic<float> peak_l_{0.0f};
	std::atomic<float> peak_r_{0.0f};
	std::atomic<float> sumsq_l_{0.0f};
	std::atomic<float> sumsq_r_{0.0f};
	std::atomic<uint64_t> rms_sample_count_{0};
	std::atomic<uint32_t> ts_discontinuity_count_{0};
	std::atomic<uint32_t> ts_discontinuity_neg_count_{0};
	// Slice 8(AC-12): CallbackMaxNs() 参照。RT スレッドからは AtomicMaxUint64 で更新するのみ。
	std::atomic<uint64_t> cb_max_ns_{0};
	// Slice 8 レビュー Medium-4: ConsumeCallbackMax2sNs() 参照。cb_max_ns_ と同じ値で更新するが、
	// 2 秒ごとに exchange でリセットされる点のみ異なる。
	std::atomic<uint64_t> cb_max_2s_ns_{0};
};

// レビュー指摘 L-11: audio_track の config.json 読み込みは output-instance.cpp の LoadConfig() へ
// 統合した(旧: audio_mix_source::LoadAudioTrackFromConfig()。device_id/device_name と合わせて
// config.json を 1 回だけ読むため、この場所には残さない)。
