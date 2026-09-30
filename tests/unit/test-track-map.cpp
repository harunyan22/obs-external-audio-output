// 対応 REQ: REQ-AUDIO-001 / Slice 1
//
// src/audio/track-map.hpp(audio_track ⇔ mix_index 変換・クランプ)の単体テスト。
// libobs に依存しないため、プラグイン本体とは別にリンクして ctest から実行する。

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../src/audio/track-map.hpp"

using namespace audio_track_map;

TEST_CASE("ClampAudioTrack: 範囲内はそのまま")
{
	bool clamped = true;
	CHECK(ClampAudioTrack(1, &clamped) == 1);
	CHECK_FALSE(clamped);

	clamped = true;
	CHECK(ClampAudioTrack(6, &clamped) == 6);
	CHECK_FALSE(clamped);

	clamped = true;
	CHECK(ClampAudioTrack(3, &clamped) == 3);
	CHECK_FALSE(clamped);
}

TEST_CASE("ClampAudioTrack: 範囲外は 1〜6 にクランプし clamped=true")
{
	bool clamped = false;
	CHECK(ClampAudioTrack(0, &clamped) == 1);
	CHECK(clamped);

	clamped = false;
	CHECK(ClampAudioTrack(-5, &clamped) == 1);
	CHECK(clamped);

	clamped = false;
	CHECK(ClampAudioTrack(7, &clamped) == 6);
	CHECK(clamped);

	clamped = false;
	CHECK(ClampAudioTrack(100, &clamped) == 6);
	CHECK(clamped);
}

TEST_CASE("ClampAudioTrack: clamped は nullptr でもクラッシュしない")
{
	CHECK(ClampAudioTrack(0) == 1);
	CHECK(ClampAudioTrack(7) == 6);
	CHECK(ClampAudioTrack(3) == 3);
}

TEST_CASE("ToMixIndex: audio_track - 1 (0-based) へ変換")
{
	CHECK(ToMixIndex(1) == 0);
	CHECK(ToMixIndex(6) == 5);
	CHECK(ToMixIndex(3) == 2);
}

TEST_CASE("ToMixIndex: 範囲外もクランプ後に変換される")
{
	bool clamped = false;
	CHECK(ToMixIndex(0, &clamped) == 0); // 1 にクランプ→ mix_index 0
	CHECK(clamped);

	clamped = false;
	CHECK(ToMixIndex(7, &clamped) == 5); // 6 にクランプ→ mix_index 5
	CHECK(clamped);
}

TEST_CASE("kDefaultAudioTrack は既定の 6")
{
	CHECK(kDefaultAudioTrack == 6);
}
