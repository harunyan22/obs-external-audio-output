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

// 対応 REQ: REQ-AUDIO-001 / Slice 1
//
// audio_track(1-based, UI/設定表記)と mix_index(0-based, libobs 表記)の変換を
// この 1 箇所に閉じる(D-01)。OBS ヘッダに依存しない単体コンパイル可能なヘッダとし、
// tests/unit/test-track-map.cpp から libobs 抜きでテストできるようにする。

#pragma once

#include <cstddef>
#include <cstdint>

namespace audio_track_map {

constexpr int kMinAudioTrack = 1;
constexpr int kMaxAudioTrack = 6;
constexpr int kDefaultAudioTrack = 6;

// audio_track を 1〜6 にクランプする。範囲外だった場合 clamped(非 null なら)に true を書く。
// 呼び出し側(非 RT コンテキスト)はこれを見て警告ログを出すこと。このヘッダ自体はログを出さない。
inline int ClampAudioTrack(int audio_track, bool *clamped = nullptr)
{
	int result = audio_track;
	bool was_clamped = false;

	if (result < kMinAudioTrack) {
		result = kMinAudioTrack;
		was_clamped = true;
	} else if (result > kMaxAudioTrack) {
		result = kMaxAudioTrack;
		was_clamped = true;
	}

	if (clamped)
		*clamped = was_clamped;

	return result;
}

// audio_track(1〜6、範囲外はクランプ)を mix_index(0〜5)へ変換する。
inline std::size_t ToMixIndex(int audio_track, bool *clamped = nullptr)
{
	return static_cast<std::size_t>(ClampAudioTrack(audio_track, clamped) - 1);
}

} // namespace audio_track_map
