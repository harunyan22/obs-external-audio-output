// 対応 REQ: REQ-DEVICE-002, REQ-DEVICE-003 / Slice 6
//
// src/output/reconnect-policy.hpp(Recovering の再接続バックオフ計算)の単体テスト。
// libobs/Windows に依存しないため、プラグイン本体とは別にリンクして ctest から実行する。

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../src/output/reconnect-policy.hpp"

TEST_CASE("初期状態: 連続失敗 0、諦めない")
{
	ReconnectPolicy policy;
	CHECK(policy.ConsecutiveFailures() == 0);
	CHECK_FALSE(policy.ShouldGiveUp());
}

TEST_CASE("バックオフ数列: 500ms -> 1s -> 2s -> 4s -> 10s(以降上限)")
{
	using namespace std::chrono_literals;

	CHECK(ReconnectPolicy::DelayForAttempt(0) == 500ms);
	CHECK(ReconnectPolicy::DelayForAttempt(1) == 1000ms);
	CHECK(ReconnectPolicy::DelayForAttempt(2) == 2000ms);
	CHECK(ReconnectPolicy::DelayForAttempt(3) == 4000ms);
	CHECK(ReconnectPolicy::DelayForAttempt(4) == 10000ms);
	// 5 段目以降は上限(10s)のまま。
	CHECK(ReconnectPolicy::DelayForAttempt(5) == 10000ms);
	CHECK(ReconnectPolicy::DelayForAttempt(9) == 10000ms);
	CHECK(ReconnectPolicy::DelayForAttempt(100) == 10000ms);
}

TEST_CASE("DelayForAttempt: 負のインデックスは 0 番目(500ms)にクランプされる")
{
	using namespace std::chrono_literals;
	CHECK(ReconnectPolicy::DelayForAttempt(-1) == 500ms);
	CHECK(ReconnectPolicy::DelayForAttempt(-100) == 500ms);
}

TEST_CASE("NotifyFailure/NextDelay: 1 回目の失敗後は 500ms、2 回目は 1s ...")
{
	using namespace std::chrono_literals;
	ReconnectPolicy policy;

	policy.NotifyFailure();
	CHECK(policy.ConsecutiveFailures() == 1);
	CHECK(policy.NextDelay() == 500ms);

	policy.NotifyFailure();
	CHECK(policy.ConsecutiveFailures() == 2);
	CHECK(policy.NextDelay() == 1000ms);

	policy.NotifyFailure();
	CHECK(policy.NextDelay() == 2000ms);

	policy.NotifyFailure();
	CHECK(policy.NextDelay() == 4000ms);

	policy.NotifyFailure();
	CHECK(policy.NextDelay() == 10000ms);
}

TEST_CASE("連続 10 回失敗で ShouldGiveUp() が true になる")
{
	ReconnectPolicy policy;
	for (int i = 0; i < ReconnectPolicy::kMaxConsecutiveFailures - 1; ++i) {
		policy.NotifyFailure();
		CHECK_FALSE(policy.ShouldGiveUp());
	}
	policy.NotifyFailure();
	CHECK(policy.ConsecutiveFailures() == ReconnectPolicy::kMaxConsecutiveFailures);
	CHECK(policy.ShouldGiveUp());
}

TEST_CASE("NotifyFailure: kMaxConsecutiveFailures を超えて呼んでもそれ以上増えない(オーバーフロー防止)")
{
	ReconnectPolicy policy;
	for (int i = 0; i < ReconnectPolicy::kMaxConsecutiveFailures + 5; ++i)
		policy.NotifyFailure();
	CHECK(policy.ConsecutiveFailures() == ReconnectPolicy::kMaxConsecutiveFailures);
	CHECK(policy.ShouldGiveUp());
}

TEST_CASE("Reset: 到着イベント相当でカウンタが 0 に戻る")
{
	ReconnectPolicy policy;
	policy.NotifyFailure();
	policy.NotifyFailure();
	policy.NotifyFailure();
	CHECK(policy.ConsecutiveFailures() == 3);

	policy.Reset();
	CHECK(policy.ConsecutiveFailures() == 0);
	CHECK_FALSE(policy.ShouldGiveUp());

	// Reset 後にまた失敗すれば数列の先頭(500ms)からやり直す。
	using namespace std::chrono_literals;
	policy.NotifyFailure();
	CHECK(policy.NextDelay() == 500ms);
}

TEST_CASE("成功→失敗→成功のサイクルを繰り返しても諦め判定に影響しない")
{
	ReconnectPolicy policy;
	for (int cycle = 0; cycle < 5; ++cycle) {
		policy.NotifyFailure();
		policy.NotifyFailure();
		CHECK_FALSE(policy.ShouldGiveUp());
		policy.Reset(); // 再接続成功。
		CHECK(policy.ConsecutiveFailures() == 0);
	}
}
