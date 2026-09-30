// 対応 REQ: REQ-COMPAT-001 / Slice 9
//
// src/compat/compat-plan.hpp(互換モードの対象ソース選定・復元プラン・自己無音判定)の単体テスト。
// libobs に依存しないため、プラグイン本体とは別にリンクして ctest から実行する
// (track-map.hpp/device-matcher.hpp と同方針)。

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../src/compat/compat-plan.hpp"

using namespace compat_plan;

// ---------------------------------------------------------------------------
// ClassifySource
// ---------------------------------------------------------------------------

TEST_CASE("ClassifySource: 選択中トラック未割り当ては NotApplicable")
{
	// mix_index=0(トラック1)向けのビットマスク(bit1=トラック2)を渡す。
	CHECK(ClassifySource(0b10, 0, MonitoringType::None) == SourceAction::NotApplicable);
	CHECK(ClassifySource(0, 0, MonitoringType::MonitorAndOutput) == SourceAction::NotApplicable);
}

TEST_CASE("ClassifySource: None は ChangeToMonitorAndOutput")
{
	CHECK(ClassifySource(0b1, 0, MonitoringType::None) == SourceAction::ChangeToMonitorAndOutput);
	// mix_index=5(トラック6)。
	CHECK(ClassifySource(0b100000, 5, MonitoringType::None) == SourceAction::ChangeToMonitorAndOutput);
}

TEST_CASE("ClassifySource: 既に MonitorAndOutput は AlreadyMonitorAndOutput(変更不要)")
{
	CHECK(ClassifySource(0b1, 0, MonitoringType::MonitorAndOutput) == SourceAction::AlreadyMonitorAndOutput);
}

TEST_CASE("ClassifySource: MonitorOnly は WarnMonitorOnly(制約1、絶対に変更しない)")
{
	CHECK(ClassifySource(0b1, 0, MonitoringType::MonitorOnly) == SourceAction::WarnMonitorOnly);
}

TEST_CASE("ClassifySource: 複数トラックに割り当て済みでも対象ビットが立っていれば分類される")
{
	// トラック1(bit0)とトラック3(bit2)の両方に割り当て。mix_index=2 で判定。
	CHECK(ClassifySource(0b101, 2, MonitoringType::None) == SourceAction::ChangeToMonitorAndOutput);
}

// ---------------------------------------------------------------------------
// IsSelfMonitorSilenceRisk(事実5)
// ---------------------------------------------------------------------------

TEST_CASE("IsSelfMonitorSilenceRisk: フラグなしなら常に false")
{
	CHECK_FALSE(IsSelfMonitorSilenceRisk(false, "default", "default"));
	CHECK_FALSE(IsSelfMonitorSilenceRisk(false, "dev-a", "dev-a"));
}

TEST_CASE("IsSelfMonitorSilenceRisk: 出力先が \"default\" ならフラグありで常に true")
{
	CHECK(IsSelfMonitorSilenceRisk(true, "default", ""));
	CHECK(IsSelfMonitorSilenceRisk(true, "default", "dev-a"));
}

TEST_CASE("IsSelfMonitorSilenceRisk: 出力先 id と既定 id が一致すれば true")
{
	CHECK(IsSelfMonitorSilenceRisk(true, "dev-a", "dev-a"));
}

TEST_CASE("IsSelfMonitorSilenceRisk: 出力先 id と既定 id が異なれば false")
{
	CHECK_FALSE(IsSelfMonitorSilenceRisk(true, "dev-a", "dev-b"));
	CHECK_FALSE(IsSelfMonitorSilenceRisk(true, "dev-a", ""));
}

// ---------------------------------------------------------------------------
// BuildEnablePlan
// ---------------------------------------------------------------------------

TEST_CASE("BuildEnablePlan: None/MonitorAndOutput/MonitorOnly/未割り当てを正しく振り分ける")
{
	std::vector<SourceInfo> sources;
	sources.push_back(SourceInfo{"mic", 0b1, MonitoringType::None, false});
	sources.push_back(SourceInfo{"desktop", 0b1, MonitoringType::MonitorAndOutput, false});
	sources.push_back(SourceInfo{"legacy", 0b1, MonitoringType::MonitorOnly, false});
	sources.push_back(SourceInfo{"other_track", 0b10, MonitoringType::None, false});

	const EnablePlan plan = BuildEnablePlan(sources, 0, "dev-out", "");

	REQUIRE(plan.to_change.size() == 1);
	CHECK(plan.to_change[0] == "mic");
	REQUIRE(plan.already_ok.size() == 1);
	CHECK(plan.already_ok[0] == "desktop");
	REQUIRE(plan.warn_monitor_only.size() == 1);
	CHECK(plan.warn_monitor_only[0] == "legacy");
	CHECK(plan.self_silence_risk.empty());
}

TEST_CASE("BuildEnablePlan: 自己無音リスクは to_change/already_ok のみが対象(WarnMonitorOnly は含めない)")
{
	std::vector<SourceInfo> sources;
	sources.push_back(SourceInfo{"desktop_audio", 0b1, MonitoringType::None, true});
	sources.push_back(SourceInfo{"already_on", 0b1, MonitoringType::MonitorAndOutput, true});
	sources.push_back(SourceInfo{"legacy_monitor_only", 0b1, MonitoringType::MonitorOnly, true});

	const EnablePlan plan = BuildEnablePlan(sources, 0, "default", "");

	REQUIRE(plan.self_silence_risk.size() == 2);
	CHECK(plan.self_silence_risk[0] == "desktop_audio");
	CHECK(plan.self_silence_risk[1] == "already_on");
}

TEST_CASE("BuildEnablePlan: 空のソース一覧は空のプランを返す")
{
	const EnablePlan plan = BuildEnablePlan({}, 0, "default", "");
	CHECK(plan.to_change.empty());
	CHECK(plan.already_ok.empty());
	CHECK(plan.warn_monitor_only.empty());
	CHECK(plan.self_silence_risk.empty());
}

// ---------------------------------------------------------------------------
// BuildRestorePlan
// ---------------------------------------------------------------------------

TEST_CASE("BuildRestorePlan: 実在するソースのみ復元対象になる(消えたソースはスキップ)")
{
	const std::vector<std::pair<std::string, MonitoringType>> saved = {
		{"mic", MonitoringType::None},
		{"deleted_source", MonitoringType::None},
	};
	const std::vector<std::string> present = {"mic"}; // deleted_source は現在存在しない。

	const std::vector<RestoreEntry> plan = BuildRestorePlan(saved, present);

	REQUIRE(plan.size() == 1);
	CHECK(plan[0].name == "mic");
	CHECK(plan[0].original_type == MonitoringType::None);
}

TEST_CASE("BuildRestorePlan: 元の monitoring type をそのまま引き継ぐ")
{
	const std::vector<std::pair<std::string, MonitoringType>> saved = {
		{"a", MonitoringType::None},
		{"b", MonitoringType::MonitorOnly},
	};
	const std::vector<std::string> present = {"a", "b"};

	const std::vector<RestoreEntry> plan = BuildRestorePlan(saved, present);

	REQUIRE(plan.size() == 2);
	CHECK(plan[0].original_type == MonitoringType::None);
	CHECK(plan[1].original_type == MonitoringType::MonitorOnly);
}

TEST_CASE("BuildRestorePlan: 保存が空なら復元プランも空")
{
	const std::vector<RestoreEntry> plan = BuildRestorePlan({}, {"mic"});
	CHECK(plan.empty());
}

TEST_CASE("BuildRestorePlan: 全ソースが消えていれば復元プランは空")
{
	const std::vector<std::pair<std::string, MonitoringType>> saved = {{"mic", MonitoringType::None}};
	const std::vector<RestoreEntry> plan = BuildRestorePlan(saved, {});
	CHECK(plan.empty());
}
