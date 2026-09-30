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

// 対応 REQ: REQ-DEVICE-002, REQ-DEVICE-003, REQ-DEVICE-004, REQ-AUDIO-007, REQ-STABILITY-001 / Slice 6
//
// OutputInstance の control thread(D-10 ③)が UI/frontend イベントスレッド・IMMNotificationClient
// コールバックから要求を受け取るための、mutex+condition_variable ベースの汎用 FIFO キュー。
// libobs/Windows API に一切依存しない純粋なテンプレートのため、tests/unit から単体テストできる
// (ring-buffer.hpp/device-matcher.hpp と同方針)。
//
// このキューは RT スレッド(libobs audio callback / WASAPI render thread)からは使わない
// (rt-audio.md: RT ループ内でのロック取得は禁止)。使用箇所は次の 3 種類のみ:
//   - UI(Qt)/frontend イベントスレッド → control thread: Start/Stop/Restart 等のコマンド enqueue。
//   - IMMNotificationClient コールバック(公式非公開スレッド)→ control thread: デバイス変化イベント。
//     wasapi-com.md/ARCHITECTURE.md §12 旧#22 の禁止 4 項目には「ロック取得禁止」は含まれておらず、
//     「ノンブロッキング必須・同期オブジェクトを待たない」が要件。Push() は短時間だけ mutex を
//     保持する通常の lock_guard(try_lock ではない)で実装するが、保持時間は「キューへの追加」のみに
//     限定し、ハンドラ側でこれ以上のブロッキング要因(cv.wait 等)を持ち込まない(このヘッダの Push()
//     はロック解放後に notify するため、コールバック自身が cv の起床を待つことはない)。
//   - control thread 自身: WaitPop() で待機しつつ、タイムアウトで戻ってウォッチドッグ/再接続の
//     周期処理を行う。

#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

template<typename T> class ControlQueue {
public:
	ControlQueue() = default;

	ControlQueue(const ControlQueue &) = delete;
	ControlQueue &operator=(const ControlQueue &) = delete;

	// 末尾へ追加する。呼び出し元がどのスレッドであっても安全(IMMNotificationClient コールバック
	// からも呼べる。上記コメント参照)。notify は mutex 解放後に行う(ロック保持時間の最小化)。
	void Push(T item)
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			queue_.push_back(std::move(item));
		}
		cv_.notify_one();
	}

	// 先頭を取り出す(あれば即座に、無ければ timeout まで待つ)。timeout で起きても空のままなら
	// std::nullopt を返す(呼び出し元はここで周期処理を行ってから再度呼ぶ想定。control thread の
	// メインループがこの形)。Shutdown() 済みでもキューに要素が残っていれば正しく取り出せる
	// (シャットダウン時のドレインを保証するため。テスト: tests/unit/test-control-queue.cpp)。
	std::optional<T> WaitPop(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || shutdown_; });
		if (queue_.empty())
			return std::nullopt;
		T item = std::move(queue_.front());
		queue_.pop_front();
		return item;
	}

	// ノンブロッキング版(空なら即座に nullopt)。IMMNotificationClient 由来のイベントを
	// control thread のメインループ内で複数個まとめて drain するのに使う。
	std::optional<T> TryPop()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (queue_.empty())
			return std::nullopt;
		T item = std::move(queue_.front());
		queue_.pop_front();
		return item;
	}

	// 待機中の WaitPop() を起こす(EXIT シーケンスで control thread のループを終了させる合図として
	// 使う想定。実際のループ終了判定は「Shutdown コマンドを積んでから呼ぶ」パターンを推奨するが、
	// このフラグ自体は「キューが空になった後は待たずに抜ける」ための補助として提供する)。
	void Shutdown()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			shutdown_ = true;
		}
		cv_.notify_all();
	}

	bool IsShutdown() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return shutdown_;
	}

	// 残っている全要素を取り出す(テスト・EXIT 時の後始末用)。
	std::vector<T> DrainAll()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		std::vector<T> result(std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
		queue_.clear();
		return result;
	}

	bool Empty() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return queue_.empty();
	}

private:
	mutable std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<T> queue_;
	bool shutdown_ = false;
};
