// 対応 REQ: REQ-AUDIO-002, REQ-NFR-002 / Slice 2
//
// src/audio/ring-buffer.hpp(SPSC 固定長リングバッファ)の単体テスト。
// libobs に依存しないため、プラグイン本体とは別にリンクして ctest から実行する。
// 実際の生産者(libobs audio thread)/消費者(WASAPI render thread)はそれぞれ 1 スレッドに
// 閉じるが、このテストはシングルスレッドから push/pop を交互に呼ぶことで
// キューとしての振る舞い(順序・満杯破棄・空・水位・ラップアラウンド・oversize)を検証する。

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../src/audio/ring-buffer.hpp"

namespace {

// テストで使う max_chunk_frames(構築時引数化されたため、テスト側で固定値を用意する。
// レビュー指摘 #2)。
constexpr uint32_t kTestMaxChunkFrames = 2048;

// frames 分のステレオ interleaved float を作り、L=value, R=-value で埋める。
void FillChunk(float *buf, uint32_t frames, float value)
{
	for (uint32_t i = 0; i < frames; ++i) {
		buf[i * 2 + 0] = value;
		buf[i * 2 + 1] = -value;
	}
}

bool ChunkEquals(const float *buf, uint32_t frames, float value)
{
	for (uint32_t i = 0; i < frames; ++i) {
		if (buf[i * 2 + 0] != value || buf[i * 2 + 1] != -value)
			return false;
	}
	return true;
}

} // namespace

TEST_CASE("TryPush/TryPop: FIFO 順序が保たれる")
{
	RingBuffer rb(4, kTestMaxChunkFrames);

	float chunk_a[8] = {};
	float chunk_b[8] = {};
	FillChunk(chunk_a, 4, 1.0f);
	FillChunk(chunk_b, 4, 2.0f);

	CHECK(rb.TryPush(chunk_a, 4));
	CHECK(rb.TryPush(chunk_b, 4));

	float out[kTestMaxChunkFrames * RingBuffer::kChannels];
	uint32_t out_frames = 0;

	CHECK(rb.TryPop(out, &out_frames));
	CHECK(out_frames == 4);
	CHECK(ChunkEquals(out, 4, 1.0f));

	CHECK(rb.TryPop(out, &out_frames));
	CHECK(out_frames == 4);
	CHECK(ChunkEquals(out, 4, 2.0f));
}

TEST_CASE("TryPop: 空なら false を返す")
{
	RingBuffer rb(4, kTestMaxChunkFrames);

	float out[kTestMaxChunkFrames * RingBuffer::kChannels];
	uint32_t out_frames = 0;
	CHECK_FALSE(rb.TryPop(out, &out_frames));
}

TEST_CASE("TryPush: 満杯なら push せず overrun カウンタが増える(既存データは破棄されない)")
{
	RingBuffer rb(4, kTestMaxChunkFrames); // capacity=4

	float chunk[2] = {};
	for (int i = 0; i < 4; ++i) {
		FillChunk(chunk, 1, (float)i);
		CHECK(rb.TryPush(chunk, 1));
	}
	CHECK(rb.OverrunCount() == 0);

	// 5 個目は満杯なので push されない。
	FillChunk(chunk, 1, 99.0f);
	CHECK_FALSE(rb.TryPush(chunk, 1));
	CHECK(rb.OverrunCount() == 1);

	// 既存の 4 個(0,1,2,3)は破棄されず順序通りに取り出せる。
	float out[kTestMaxChunkFrames * RingBuffer::kChannels];
	uint32_t out_frames = 0;
	for (int i = 0; i < 4; ++i) {
		CHECK(rb.TryPop(out, &out_frames));
		CHECK(out_frames == 1);
		CHECK(ChunkEquals(out, 1, (float)i));
	}
	CHECK_FALSE(rb.TryPop(out, &out_frames));
}

TEST_CASE("Level: push/pop に応じて水位が増減する")
{
	RingBuffer rb(4, kTestMaxChunkFrames);
	CHECK(rb.Level() == 0);

	float chunk[2] = {1.0f, -1.0f};
	CHECK(rb.TryPush(chunk, 1));
	CHECK(rb.Level() == 1);
	CHECK(rb.TryPush(chunk, 1));
	CHECK(rb.Level() == 2);

	float out[kTestMaxChunkFrames * RingBuffer::kChannels];
	uint32_t out_frames = 0;
	CHECK(rb.TryPop(out, &out_frames));
	CHECK(rb.Level() == 1);
}

TEST_CASE("ラップアラウンド: capacity を超えて push/pop を繰り返しても順序が保たれる")
{
	RingBuffer rb(4, kTestMaxChunkFrames); // capacity=4(2 の冪)

	float chunk[2] = {};
	float out[kTestMaxChunkFrames * RingBuffer::kChannels];
	uint32_t out_frames = 0;

	// write_index_/read_index_ が capacity を何周も超えるまで push→pop を繰り返す。
	for (int cycle = 0; cycle < 20; ++cycle) {
		float value = (float)cycle;
		FillChunk(chunk, 1, value);
		CHECK(rb.TryPush(chunk, 1));
		CHECK(rb.TryPop(out, &out_frames));
		CHECK(out_frames == 1);
		CHECK(ChunkEquals(out, 1, value));
	}
	CHECK(rb.Level() == 0);
	CHECK(rb.OverrunCount() == 0);
}

TEST_CASE("TryPush: max_chunk_frames 超過は切り詰めて oversize カウンタが増える")
{
	RingBuffer rb(4, kTestMaxChunkFrames);

	// max_chunk_frames を超える frames を要求する(実データはゼロ埋めで十分、切り詰め挙動のみ検証)。
	static float oversized[(kTestMaxChunkFrames + 16) * RingBuffer::kChannels] = {};
	CHECK(rb.TryPush(oversized, kTestMaxChunkFrames + 16));
	CHECK(rb.OversizeCount() == 1);

	float out[kTestMaxChunkFrames * RingBuffer::kChannels];
	uint32_t out_frames = 0;
	CHECK(rb.TryPop(out, &out_frames));
	CHECK(out_frames == kTestMaxChunkFrames);
}

TEST_CASE("PushSilence: 無音チャンクが積まれ pop 側は 0 埋めを読める")
{
	RingBuffer rb(4, kTestMaxChunkFrames);

	CHECK(rb.PushSilence(4));
	CHECK(rb.Level() == 1);

	float out[kTestMaxChunkFrames * RingBuffer::kChannels];
	uint32_t out_frames = 0;
	CHECK(rb.TryPop(out, &out_frames));
	CHECK(out_frames == 4);
	CHECK(ChunkEquals(out, 4, 0.0f));
}

TEST_CASE("Reset: 水位とカウンタが 0 に戻る")
{
	RingBuffer rb(4, kTestMaxChunkFrames);

	float chunk[2] = {1.0f, -1.0f};
	CHECK(rb.TryPush(chunk, 1));
	CHECK(rb.TryPush(chunk, 1));
	CHECK(rb.TryPush(chunk, 1));
	CHECK(rb.TryPush(chunk, 1));
	CHECK_FALSE(rb.TryPush(chunk, 1)); // overrun を 1 発生させる
	CHECK(rb.OverrunCount() == 1);

	rb.Reset();

	CHECK(rb.Level() == 0);
	CHECK(rb.OverrunCount() == 0);
	CHECK(rb.OversizeCount() == 0);

	float out[kTestMaxChunkFrames * RingBuffer::kChannels];
	uint32_t out_frames = 0;
	CHECK_FALSE(rb.TryPop(out, &out_frames));
}

TEST_CASE("PeakLevel: push で更新された最大水位を pop 後も保持する(Slice 8, AC-12)")
{
	RingBuffer rb(4, kTestMaxChunkFrames);
	CHECK(rb.PeakLevel() == 0);

	float chunk[2] = {1.0f, -1.0f};
	CHECK(rb.TryPush(chunk, 1));
	CHECK(rb.TryPush(chunk, 1));
	CHECK(rb.TryPush(chunk, 1));
	CHECK(rb.PeakLevel() == 3);

	float out[kTestMaxChunkFrames * RingBuffer::kChannels];
	uint32_t out_frames = 0;
	CHECK(rb.TryPop(out, &out_frames));
	CHECK(rb.TryPop(out, &out_frames));
	// 水位は 1 まで下がるが、PeakLevel() は過去の最大値(3)を保持したまま。
	CHECK(rb.Level() == 1);
	CHECK(rb.PeakLevel() == 3);

	// その後 push しても既存の最大値を下回るなら更新されない。
	CHECK(rb.TryPush(chunk, 1));
	CHECK(rb.Level() == 2);
	CHECK(rb.PeakLevel() == 3);
}

TEST_CASE("PeakLevel: Reset() で 0 に戻る(Slice 8, AC-12)")
{
	RingBuffer rb(4, kTestMaxChunkFrames);

	float chunk[2] = {1.0f, -1.0f};
	CHECK(rb.TryPush(chunk, 1));
	CHECK(rb.TryPush(chunk, 1));
	CHECK(rb.PeakLevel() == 2);

	rb.Reset();
	CHECK(rb.PeakLevel() == 0);
}

TEST_CASE("Capacity: 2 の冪ならそのまま返す")
{
	RingBuffer rb(8, kTestMaxChunkFrames);
	CHECK(rb.Capacity() == 8);
}

TEST_CASE("Capacity: 2 の冪でない容量は次の 2 の冪へ切り上げられる(レビュー指摘 #7)")
{
	RingBuffer rb3(3, kTestMaxChunkFrames);
	CHECK(rb3.Capacity() == 4);

	RingBuffer rb5(5, kTestMaxChunkFrames);
	CHECK(rb5.Capacity() == 8);

	RingBuffer rb1(1, kTestMaxChunkFrames);
	CHECK(rb1.Capacity() == 1);
}

TEST_CASE("MaxChunkFrames: コンストラクタで渡した値を返す")
{
	RingBuffer rb(4, 1500);
	CHECK(rb.MaxChunkFrames() == 1500);
}

TEST_CASE("Capacity: 極端な値は kMaxCapacityChunks にクランプされ 0 にならない(レビュー指摘 #6)")
{
	// NextPowerOfTwo の素朴な実装は v が 2^31 を超えると v+1 が uint32_t を一周して 0 になり得る。
	// クランプにより capacity_ が 0 になることなく、常に有効な 2 の冪を返すことを確認する。
	// kMaxCapacityChunks(1<<20)分のスロットを実際に確保するため、ここでは max_chunk_frames を
	// 最小(1 frame)にしてテストのメモリ使用量を抑える(RingBuffer は capacity_ * max_chunk_frames
	// に比例したメモリを構築時に確保するため)。
	constexpr uint32_t kMinChunkFrames = 1;

	RingBuffer rb_max(UINT32_MAX, kMinChunkFrames);
	CHECK(rb_max.Capacity() == RingBuffer::kMaxCapacityChunks);
	CHECK(rb_max.Capacity() != 0);

	RingBuffer rb_boundary((1u << 31) + 1, kMinChunkFrames);
	CHECK(rb_boundary.Capacity() == RingBuffer::kMaxCapacityChunks);

	RingBuffer rb_at_limit(RingBuffer::kMaxCapacityChunks, kMinChunkFrames);
	CHECK(rb_at_limit.Capacity() == RingBuffer::kMaxCapacityChunks);
}
