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

// 対応 REQ: REQ-AUDIO-002, REQ-NFR-002 / Slice 2
//
// SPSC(単一書き手・単一読み手)固定長リングバッファ(rt-audio.md 準拠)。
// 書き手 = libobs audio thread(AudioMixSource::HandleAudioData から TryPush)。
// 読み手 = WASAPI render thread(WasapiOutput::RenderLoop から TryPop)。
// 両者ともヒープ確保・ロック・ブロックなしで呼べる(コンストラクタで一括確保、動的拡張禁止)。
// OBS ヘッダに依存しないため tests/unit から libobs 抜きで単体テストできる(track-map.hpp と同方針)。

#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>

class RingBuffer {
public:
	// S2 の出力は常にステレオ(AudioMixSource の audio_convert_info.speakers = SPEAKERS_STEREO 固定)。
	// この static_assert はチャンネル数を変更する際に SPEAKERS_STEREO 側の追従を忘れないための
	// アンカー(レビュー指摘 #14)。ステレオ以外に対応する際は audio-mix-source.cpp の
	// audio_convert_info.speakers と WasapiOutput::WriteFrames の L/R 配置を合わせて見直すこと。
	static constexpr uint32_t kChannels = 2;
	static_assert(kChannels == 2, "kChannels must match AudioMixSource's audio_convert_info.speakers "
				      "(SPEAKERS_STEREO); update both together if this changes.");

	// capacity に渡せる実質上限(2 の冪)。NextPowerOfTwo() のオーバーフロー防止クランプに使う
	// (レビュー指摘 #6)。約 100 万チャンクあれば通常の Start() 経路(プリロール計算)で必要になる
	// チャンク数を大きく超えるため、実用上のクランプとして十分な余裕がある。
	static constexpr uint32_t kMaxCapacityChunks = 1u << 20;

	// capacity: チャンク数。2 の冪でなくても構わない(コンストラクタ内で次の 2 の冪へ切り上げる。
	// ラップアラウンドを & マスクで行うための内部要件。レビュー指摘 #7)。kMaxCapacityChunks を
	// 超える値は kMaxCapacityChunks にクランプされる(レビュー指摘 #6: v+1 が uint32_t をオーバー
	// フローして 0 になり capacity_ が 0 になる事故を防ぐ)。
	// max_chunk_frames: 1 スロットが保持できる最大 frames(構築時引数。レビュー指摘 #2。呼び出し側
	// (OutputInstance)がデバイスレートから ceil(1024 * device_rate / obs_rate) + マージン で計算する)。
	// 超過分は切り詰めて oversize カウンタ++(#11: リサンプル時に frames が可変になることへの備え)。
	RingBuffer(uint32_t capacity, uint32_t max_chunk_frames)
		: capacity_(NextPowerOfTwo(capacity)),
		  max_chunk_frames_(max_chunk_frames),
		  slots_(new Slot[capacity_])
	{
		assert(capacity > 0 && "RingBuffer capacity must be > 0");
		assert(max_chunk_frames_ > 0 && "RingBuffer max_chunk_frames must be > 0");
		for (uint32_t i = 0; i < capacity_; ++i)
			slots_[i].samples = std::make_unique<float[]>((size_t)max_chunk_frames_ * kChannels);
	}

	~RingBuffer() = default;

	RingBuffer(const RingBuffer &) = delete;
	RingBuffer &operator=(const RingBuffer &) = delete;

	// 書き手専用(RT: libobs audio thread)。frames > max_chunk_frames_ は max_chunk_frames_ に
	// 切り詰めて oversize カウンタ++。満杯なら push せず false を返し overrun カウンタ++
	// (ブロックしない)。
	bool TryPush(const float *interleaved, uint32_t frames)
	{
		const uint32_t w = write_index_.load(std::memory_order_relaxed);
		const uint32_t r = read_index_.load(std::memory_order_acquire);
		if (w - r >= capacity_) {
			overrun_count_.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		uint32_t copy_frames = frames;
		if (copy_frames > max_chunk_frames_) {
			copy_frames = max_chunk_frames_;
			oversize_count_.fetch_add(1, std::memory_order_relaxed);
		}

		Slot &slot = slots_[w & (capacity_ - 1)];
		std::memcpy(slot.samples.get(), interleaved, (size_t)copy_frames * kChannels * sizeof(float));
		slot.frames = copy_frames;

		write_index_.store(w + 1, std::memory_order_release);
		// Slice 8(AC-12、ring_peak の≤90%判定用): この push 直後の水位で最大水位を更新する。RT 安全
		// (atomic load/CAS のみ、ヒープ確保・ロックなし)。
		// レビュー指摘 Low-12(L-1 の提案どおり): 2 回目の read_index_ acquire load はせず、関数冒頭で
		// 既に読んだ r をそのまま使う(SPSC で読み手は read_index_ を単調増加させるだけなので r は
		// 「push 直後時点の実際の read_index_」以下の値。w+1-r は実際の水位以上にしかならない
		// 過大側近似だが、冒頭の w-r<capacity_ チェックにより w+1-r は必ず capacity_ 以下に収まるため
		// PeakLevel() が capacity_ を超えて報告されることはない。水位を小さく見誤ることはない
		// (peak=最大値の記録として安全側)。
		UpdatePeakLevel((w + 1 >= r) ? (w + 1 - r) : 0);
		return true;
	}

	// 無音チャンクを 1 つ積む(プリロール専用)。OutputInstance::Start() から Start 前・単一スレッドで
	// 呼ぶ想定(RT ではないが TryPush と同じ非ブロッキング経路を使う)。
	// レビュー指摘 Low-11: TryPush() と異なり PeakLevel() は更新しない(意図的)。プリロールは
	// Running 状態に入る前に一括で積む定型処理であり、その水位を「実運用中の最大水位」として
	// PeakLevel()/AC-12 の ring_peak 判定に混ぜると、実際のトラフィックとは無関係にプリロール分だけで
	// 高い値が記録されてしまうため。
	bool PushSilence(uint32_t frames)
	{
		const uint32_t w = write_index_.load(std::memory_order_relaxed);
		const uint32_t r = read_index_.load(std::memory_order_acquire);
		if (w - r >= capacity_) {
			// プリロールは Running 状態に入る前(OutputInstance::Start())にのみ呼ばれる。
			// overrun カウンタは Running 状態に入ってから計上する方針(rt-audio.md)のため、
			// ここでは計上しない(レビュー指摘 L-8)。呼び出し側が戻り値 false を見て警告ログを
			// 出す(OutputInstance::Start() 参照)。
			return false;
		}

		const uint32_t copy_frames = frames > max_chunk_frames_ ? max_chunk_frames_ : frames;
		Slot &slot = slots_[w & (capacity_ - 1)];
		std::memset(slot.samples.get(), 0, (size_t)copy_frames * kChannels * sizeof(float));
		slot.frames = copy_frames;

		write_index_.store(w + 1, std::memory_order_release);
		return true;
	}

	// 読み手専用(RT: WASAPI render thread)。out_interleaved は少なくとも
	// MaxChunkFrames() * kChannels 個の float を書き込める領域であること。
	// 空なら false を返す(無音出力+underrun カウンタの計上判断は呼び出し側の責務。
	// rt-audio.md: underrun は Running 状態に入ってからのみ計上する)。
	bool TryPop(float *out_interleaved, uint32_t *out_frames)
	{
		const uint32_t r = read_index_.load(std::memory_order_relaxed);
		const uint32_t w = write_index_.load(std::memory_order_acquire);
		if (r == w)
			return false;

		Slot &slot = slots_[r & (capacity_ - 1)];
		const uint32_t frames = slot.frames;
		std::memcpy(out_interleaved, slot.samples.get(), (size_t)frames * kChannels * sizeof(float));
		if (out_frames)
			*out_frames = frames;

		read_index_.store(r + 1, std::memory_order_release);
		return true;
	}

	// Start() のたびに空の状態へ戻す(state_mutex_ 配下・単一スレッドから呼ぶ想定。RT ではない)。
	void Reset()
	{
		write_index_.store(0, std::memory_order_relaxed);
		read_index_.store(0, std::memory_order_relaxed);
		overrun_count_.store(0, std::memory_order_relaxed);
		oversize_count_.store(0, std::memory_order_relaxed);
		peak_level_.store(0, std::memory_order_relaxed);
	}

	uint32_t Capacity() const { return capacity_; }
	uint32_t MaxChunkFrames() const { return max_chunk_frames_; }

	// Slice 8(AC-12): TryPush() が観測した最大水位(チャンク数、Capacity() と同じ単位)。
	// このインスタンスの生存期間(直近の Reset() 以降)を通じた最大値で、Level() のような
	// その場の値ではない。
	uint32_t PeakLevel() const { return peak_level_.load(std::memory_order_relaxed); }

	// 充填チャンク数(水位)。診断用途の概算値であり、read/write の非同期な読み取りにより
	// 呼び出しタイミングで多少ぶれ得る(RT スレッドをブロックしない)。read→write の順に load し、
	// 範囲外(その間に read が write を追い越して観測されるアンダーフロー)は 0 に clamp する
	// (レビュー指摘 #9)。
	uint32_t Level() const
	{
		const uint32_t r = read_index_.load(std::memory_order_acquire);
		const uint32_t w = write_index_.load(std::memory_order_acquire);
		const uint32_t diff = w - r;
		return diff <= capacity_ ? diff : 0;
	}

	uint64_t OverrunCount() const { return overrun_count_.load(std::memory_order_relaxed); }
	uint64_t OversizeCount() const { return oversize_count_.load(std::memory_order_relaxed); }

private:
	struct Slot {
		std::unique_ptr<float[]> samples;
		uint32_t frames = 0;
	};

	static uint32_t NextPowerOfTwo(uint32_t v)
	{
		if (v <= 1)
			return 1;
		// v がオーバーフロー域(2^31 超)にあると、下のビット埋め+1 で uint32_t が一周して 0 に
		// なり capacity_ == 0 という壊れた状態になり得る。先に上限でクランプすることでこれを
		// 防ぐ(レビュー指摘 #6)。kMaxCapacityChunks 自体が 2 の冪なのでそのまま返せる。
		if (v > kMaxCapacityChunks)
			return kMaxCapacityChunks;
		v -= 1;
		v |= v >> 1;
		v |= v >> 2;
		v |= v >> 4;
		v |= v >> 8;
		v |= v >> 16;
		return v + 1;
	}

	// Slice 8: PeakLevel() 用の CAS ループ(TryPush() の書き手からのみ呼ばれる。RT 安全)。
	void UpdatePeakLevel(uint32_t candidate)
	{
		uint32_t current = peak_level_.load(std::memory_order_relaxed);
		while (candidate > current &&
		       !peak_level_.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
		}
	}

	const uint32_t capacity_;
	const uint32_t max_chunk_frames_;
	std::unique_ptr<Slot[]> slots_;

	std::atomic<uint32_t> write_index_{0};
	std::atomic<uint32_t> read_index_{0};
	std::atomic<uint64_t> overrun_count_{0};
	std::atomic<uint64_t> oversize_count_{0};
	std::atomic<uint32_t> peak_level_{0};
};
