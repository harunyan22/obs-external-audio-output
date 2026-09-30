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

// 対応 REQ: REQ-DEVICE-002, REQ-DEVICE-003 / Slice 6
//
// Recovering 状態での再接続バックオフ計算(D-12)。純粋ロジックで libobs/Windows 非依存のため
// tests/unit から単体テストできる(control-queue.hpp と同方針)。OutputInstance の control thread
// のみが読み書きする(RT ではない、外部ロック不要)。
//
// 数列: 500ms → 1s → 2s → 4s → 10s(以降上限)。連続 10 回失敗で「諦める」(呼び出し元が Failed へ
// 遷移する)。到着イベント(再接続成功、または OnDeviceAdded/OnDeviceStateChanged(ACTIVE))で
// Reset() を呼ぶとカウンタが 0 に戻る。

#pragma once

#include <chrono>
#include <cstddef>

class ReconnectPolicy {
public:
	// 連続失敗の上限(これに達したら ShouldGiveUp() が true。ブリーフ: 「連続 10 回」)。
	static constexpr int kMaxConsecutiveFailures = 10;

	// 遅延の数列(ms)。5 段目以降は最後の値(10000ms)を使い続ける(DelayForAttempt 参照)。
	static constexpr std::chrono::milliseconds kDelaySequence[] = {
		std::chrono::milliseconds(500), std::chrono::milliseconds(1000), std::chrono::milliseconds(2000),
		std::chrono::milliseconds(4000), std::chrono::milliseconds(10000)};

	// attempt_index(0-indexed: 1 回目の失敗後の待ち時間が index=0)に対応する遅延を返す。
	// 範囲外(負値/kDelaySequence を超える)は端にクランプする。単体テスト・NextDelay() の両方から使う
	// 純粋関数。
	static std::chrono::milliseconds DelayForAttempt(int attempt_index)
	{
		constexpr size_t kCount = sizeof(kDelaySequence) / sizeof(kDelaySequence[0]);
		if (attempt_index < 0)
			attempt_index = 0;
		size_t idx = (size_t)attempt_index;
		if (idx >= kCount)
			idx = kCount - 1;
		return kDelaySequence[idx];
	}

	// 再接続成功、またはデバイス到着イベント(OnDeviceAdded/OnDeviceStateChanged(ACTIVE))で呼ぶ
	// (ブリーフ実装内容 8: 「バックオフをリセットして即試行」)。
	void Reset() { consecutive_failures_ = 0; }

	// 1 回の再接続試行が失敗するたびに呼ぶ。kMaxConsecutiveFailures に達した後はそれ以上増やさない
	// (呼び出し元は ShouldGiveUp() を見て Failed へ遷移し、以後 NotifyFailure() を呼ばない想定だが、
	// 万一続けて呼ばれても frames_since_last_correction_ 相当のオーバーフローは起きない)。
	void NotifyFailure()
	{
		if (consecutive_failures_ < kMaxConsecutiveFailures)
			++consecutive_failures_;
	}

	int ConsecutiveFailures() const { return consecutive_failures_; }

	// 連続 kMaxConsecutiveFailures 回失敗したか(呼び出し元はここで Recovering → Failed へ遷移する)。
	bool ShouldGiveUp() const { return consecutive_failures_ >= kMaxConsecutiveFailures; }

	// 直近の失敗回数から「次の試行までの待ち時間」を返す。NotifyFailure() を 1 回以上呼んだ後
	// (ShouldGiveUp()==false の間)にのみ意味を持つ。呼び出し元は NextDelay() 経過後に次の再接続を
	// 試みる。
	std::chrono::milliseconds NextDelay() const { return DelayForAttempt(consecutive_failures_ - 1); }

private:
	int consecutive_failures_ = 0;
};
