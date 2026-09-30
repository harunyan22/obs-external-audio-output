// 対応 REQ: REQ-DEVICE-001 / Slice 4
//
// src/device/device-matcher.hpp(D-14 のデバイス解決ロジック)の単体テスト。
// libobs/COM に依存しないため、プラグイン本体とは別にリンクして ctest から実行する。

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../src/device/device-matcher.hpp"

using namespace device_matcher;

namespace {

std::vector<DeviceEntry> SampleDevices()
{
	return {
		{"id-speakers", "Speakers (Realtek)"},
		{"id-headphones", "Headphones (USB Audio)"},
	};
}

} // namespace

TEST_CASE("Resolve: device_id=\"default\" は無条件で ResolvedBy::Default")
{
	const auto result = Resolve("default", "", SampleDevices());
	REQUIRE(result.has_value());
	CHECK(result->id == "default");
	CHECK(result->resolved_by == ResolvedBy::Default);
}

TEST_CASE("Resolve: device_id=\"default\" は devices が空でも解決成功")
{
	const auto result = Resolve("default", "", {});
	REQUIRE(result.has_value());
	CHECK(result->id == "default");
	CHECK(result->resolved_by == ResolvedBy::Default);
}

TEST_CASE("Resolve: id 一致は ResolvedBy::ById")
{
	const auto result = Resolve("id-headphones", "stale name", SampleDevices());
	REQUIRE(result.has_value());
	CHECK(result->id == "id-headphones");
	CHECK(result->name == "Headphones (USB Audio)");
	CHECK(result->resolved_by == ResolvedBy::ById);
}

TEST_CASE("Resolve: id 不一致でも名前一致すれば ResolvedBy::ByName で新しい id を返す")
{
	const auto result = Resolve("id-old-stale", "Speakers (Realtek)", SampleDevices());
	REQUIRE(result.has_value());
	CHECK(result->id == "id-speakers"); // 新しい id を採用
	CHECK(result->name == "Speakers (Realtek)");
	CHECK(result->resolved_by == ResolvedBy::ByName);
}

TEST_CASE("Resolve: id も名前も不一致なら未解決(nullopt)")
{
	const auto result = Resolve("id-old-stale", "Nonexistent Device", SampleDevices());
	CHECK_FALSE(result.has_value());
}

TEST_CASE("Resolve: 名前が空文字列なら名前一致を試みない")
{
	const auto result = Resolve("id-old-stale", "", SampleDevices());
	CHECK_FALSE(result.has_value());
}

TEST_CASE("Resolve: devices が空リストで id 指定なら未解決")
{
	const auto result = Resolve("id-headphones", "Headphones (USB Audio)", {});
	CHECK_FALSE(result.has_value());
}

TEST_CASE("Resolve: id 一致が名前一致より優先される")
{
	// id-speakers は Speakers (Realtek) だが、保存名は別デバイスの名前。id 一致を優先し、
	// 保存名は無視されて id 一致側の実際の名前が返る。
	const auto result = Resolve("id-speakers", "Headphones (USB Audio)", SampleDevices());
	REQUIRE(result.has_value());
	CHECK(result->id == "id-speakers");
	CHECK(result->name == "Speakers (Realtek)");
	CHECK(result->resolved_by == ResolvedBy::ById);
}

TEST_CASE("kDefaultDeviceId は \"default\"")
{
	CHECK(std::string(kDefaultDeviceId) == "default");
}

TEST_CASE("Resolve: 同名デバイスが複数あれば名前一致でも未解決(AmbiguousName)")
{
	const std::vector<DeviceEntry> devices = {
		{"id-usb-1", "USB Audio Device"},
		{"id-usb-2", "USB Audio Device"}, // 同名の別デバイス
	};

	UnresolvedReason reason = UnresolvedReason::NoMatch;
	std::size_t ambiguous_count = 0;
	const auto result = Resolve("id-old-stale", "USB Audio Device", devices, &reason, &ambiguous_count);

	CHECK_FALSE(result.has_value());
	CHECK(reason == UnresolvedReason::AmbiguousName);
	CHECK(ambiguous_count == 2);
}

TEST_CASE("Resolve: 名前不一致(NoMatch)は out_reason にもその旨が入る")
{
	UnresolvedReason reason = UnresolvedReason::AmbiguousName;
	const auto result = Resolve("id-old-stale", "Nonexistent Device", SampleDevices(), &reason);

	CHECK_FALSE(result.has_value());
	CHECK(reason == UnresolvedReason::NoMatch);
}
