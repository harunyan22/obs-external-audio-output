// 対応 REQ: REQ-DEVICE-002, REQ-DEVICE-003, REQ-DEVICE-004, REQ-STABILITY-001 / Slice 6
//
// src/output/control-queue.hpp(control thread 向けの mutex+condition_variable ベース FIFO キュー)の
// 単体テスト。libobs/Windows に依存しないため、プラグイン本体とは別にリンクして ctest から実行する。
// 実際の生産者(UI/frontend イベントスレッド、IMMNotificationClient コールバック)/消費者(control
// thread)はそれぞれ複数スレッドから push/pop する想定だが、このテストはシングルスレッドから
// Push/WaitPop/TryPop/DrainAll を呼ぶことで FIFO 順序・空/タイムアウト・シャットダウン時のドレインを
// 検証する(スレッド安全性そのもの(データレースの不在)は型/実装のレビューで担保する対象とし、
// このテストは振る舞いの正しさを検証する)。

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../src/output/control-queue.hpp"

#include <chrono>
#include <thread>
#include <vector>

TEST_CASE("Push/WaitPop: FIFO 順序が保たれる")
{
	ControlQueue<int> queue;
	queue.Push(1);
	queue.Push(2);
	queue.Push(3);

	CHECK(queue.WaitPop(std::chrono::milliseconds(10)) == 1);
	CHECK(queue.WaitPop(std::chrono::milliseconds(10)) == 2);
	CHECK(queue.WaitPop(std::chrono::milliseconds(10)) == 3);
}

TEST_CASE("WaitPop: 空のままタイムアウトすると std::nullopt を返す")
{
	ControlQueue<int> queue;
	const auto result = queue.WaitPop(std::chrono::milliseconds(20));
	CHECK_FALSE(result.has_value());
}

TEST_CASE("TryPop: 空なら即座に std::nullopt、要素があれば取り出す")
{
	ControlQueue<int> queue;
	CHECK_FALSE(queue.TryPop().has_value());

	queue.Push(42);
	const auto result = queue.TryPop();
	REQUIRE(result.has_value());
	CHECK(*result == 42);
	CHECK_FALSE(queue.TryPop().has_value());
}

TEST_CASE("Empty: push/pop に応じて増減する")
{
	ControlQueue<int> queue;
	CHECK(queue.Empty());
	queue.Push(1);
	CHECK_FALSE(queue.Empty());
	queue.TryPop();
	CHECK(queue.Empty());
}

TEST_CASE("DrainAll: 残っている全要素を FIFO 順で取り出し、キューは空になる")
{
	ControlQueue<int> queue;
	queue.Push(10);
	queue.Push(20);
	queue.Push(30);

	const std::vector<int> drained = queue.DrainAll();
	REQUIRE(drained.size() == 3);
	CHECK(drained[0] == 10);
	CHECK(drained[1] == 20);
	CHECK(drained[2] == 30);
	CHECK(queue.Empty());
}

TEST_CASE("Shutdown: 待機中の WaitPop が起きる(要素が無ければ nullopt)")
{
	ControlQueue<int> queue;
	CHECK_FALSE(queue.IsShutdown());

	// Shutdown() は別スレッドから呼ばれる想定(EXIT シーケンス)だが、単体テストでは同期的に検証する:
	// Shutdown 済みのキューへ WaitPop すると、要素が無ければ即座に(タイムアウトを待たず)nullopt を
	// 返すのが期待動作。
	queue.Shutdown();
	CHECK(queue.IsShutdown());

	const auto start = std::chrono::steady_clock::now();
	const auto result = queue.WaitPop(std::chrono::seconds(5));
	const auto elapsed = std::chrono::steady_clock::now() - start;
	CHECK_FALSE(result.has_value());
	// 5 秒のタイムアウトを最後まで待たされていないこと(Shutdown 済みなら cv.wait_for の述語が
	// 即座に true になるため)。
	CHECK(elapsed < std::chrono::seconds(1));
}

TEST_CASE("Shutdown 後でもキューに残っている要素は WaitPop/TryPop で正しくドレインできる")
{
	ControlQueue<int> queue;
	queue.Push(1);
	queue.Push(2);
	queue.Shutdown();

	// シャットダウン後も既存の要素は失われない(EXIT シーケンスでキューに積まれたコマンドを
	// 取りこぼさないことを保証する)。
	CHECK(queue.WaitPop(std::chrono::milliseconds(10)) == 1);
	CHECK(queue.WaitPop(std::chrono::milliseconds(10)) == 2);
	CHECK_FALSE(queue.WaitPop(std::chrono::milliseconds(10)).has_value());
}

TEST_CASE("2 スレッド間の Push/WaitPop: 生産者が push した全要素を消費者が順序どおり受け取る")
{
	ControlQueue<int> queue;
	constexpr int kCount = 200;

	std::thread producer([&queue] {
		for (int i = 0; i < kCount; ++i)
			queue.Push(i);
	});

	std::vector<int> received;
	received.reserve(kCount);
	while ((int)received.size() < kCount) {
		auto item = queue.WaitPop(std::chrono::milliseconds(500));
		if (item)
			received.push_back(*item);
	}

	producer.join();

	REQUIRE(received.size() == (size_t)kCount);
	for (int i = 0; i < kCount; ++i)
		CHECK(received[(size_t)i] == i);
}
