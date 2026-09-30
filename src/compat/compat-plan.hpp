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

// 対応 REQ: REQ-COMPAT-001 / Slice 9
//
// 互換モード(output_mode="obs_monitor_compat")の純粋ロジック(libobs 非依存)。
// device-matcher.hpp/track-map.hpp と同方針: このヘッダは obs.h に一切依存しないため、
// tests/unit/test-compat-plan.cpp から libobs 抜きで単体テストできる。実際の obs_* API 呼び出しは
// src/compat/monitor-compat-controller.cpp(libobs 依存)が担う。
//
// 扱う 3 つの判断(slice-9.md 実装変更範囲):
//   1. 対象ソース選定: mixers ビットマスク × mix_index × 現 monitoring type →
//      変更する(None→MONITOR_AND_OUTPUT)/ 変更不要(既に MONITOR_AND_OUTPUT)/ 警告(MONITOR_ONLY)。
//      REQ-COMPAT-001 制約 1: MONITOR_ONLY のソースは絶対に変更しない(AC-04/AC-26b の
//      bit-identical を守るため。MONITOR_ONLY のみがミックスに影響する、ARCHITECTURE.md §12 事実 1)。
//   2. 復元プラン: 保存済み {ソース名 -> 元 monitoring type} と、現在実在するソース名の集合から、
//      実際に復元すべき対象を返す(消えたソースはスキップ、REQ-COMPAT-001 制約 2)。
//   3. 自己無音判定(事実 5): 出力先 device_id が既定デバイスと同一(または device_id=="default")かつ
//      ソースが OBS_SOURCE_DO_NOT_SELF_MONITOR フラグを持つ場合、そのソースは無音になり得る。

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace compat_plan {

// obs_monitoring_type(libobs/obs.h)と値を 1:1 で対応させる(0/1/2)。monitor-compat-controller.cpp
// が static_cast で相互変換する。このヘッダ自体は obs.h を include しない。
enum class MonitoringType {
	None = 0,
	MonitorOnly = 1,
	MonitorAndOutput = 2,
};

// 選択中トラックに対する 1 ソースの分類結果。
enum class SourceAction {
	NotApplicable,            // 選択中トラック(mix_index)に未割り当て
	AlreadyMonitorAndOutput,  // 既に MONITOR_AND_OUTPUT(変更不要。復元対象にもしない)
	ChangeToMonitorAndOutput, // None → MONITOR_AND_OUTPUT へ変更する(復元対象)
	WarnMonitorOnly,          // MONITOR_ONLY のまま変更しない(制約 1、警告リストへ)
};

// mixers_bitmask/mix_index から「選択中トラックに割り当てられているか」を判定したうえで分類する。
// mix_index は 0〜5(audio_track_map::ToMixIndex() 済みの値を渡すこと。このヘッダは audio_track の
// クランプを行わない)。
inline SourceAction ClassifySource(uint32_t mixers_bitmask, std::size_t mix_index, MonitoringType monitoring_type)
{
	const uint32_t bit = 1u << static_cast<uint32_t>(mix_index);
	if ((mixers_bitmask & bit) == 0)
		return SourceAction::NotApplicable;

	switch (monitoring_type) {
	case MonitoringType::MonitorOnly:
		return SourceAction::WarnMonitorOnly;
	case MonitoringType::MonitorAndOutput:
		return SourceAction::AlreadyMonitorAndOutput;
	case MonitoringType::None:
	default:
		return SourceAction::ChangeToMonitorAndOutput;
	}
}

// 事実 5(ARCHITECTURE.md §12): デスクトップ音声キャプチャ(OBS_SOURCE_DO_NOT_SELF_MONITOR フラグ)は
// モニタリング先がキャプチャ元と同一物理デバイスのとき自己ループバック防止で無音になる。
// slice-9.md の簡略化方針どおり「出力先 id と既定 id の一致」で判定する(個々のソースが実際にどの
// デバイスをキャプチャしているかまでは立ち入らない)。output_device_id=="default" は常に「その時点の
// 既定デバイスと同一」を意味するため無条件で true(has_do_not_self_monitor が true の場合)。
inline bool IsSelfMonitorSilenceRisk(bool has_do_not_self_monitor, const std::string &output_device_id,
				     const std::string &default_device_id)
{
	if (!has_do_not_self_monitor)
		return false;
	if (output_device_id == "default")
		return true;
	return !default_device_id.empty() && output_device_id == default_device_id;
}

// EnumerateSourceInfos() 相当の入力 1 件(monitor-compat-controller.cpp が obs_enum_sources から
// 詰め替えて渡す)。
struct SourceInfo {
	std::string name;
	uint32_t mixers_bitmask = 0;
	MonitoringType monitoring_type = MonitoringType::None;
	bool has_do_not_self_monitor = false;
};

// 有効化/再適用の 1 回分の分類結果。to_change が実際に obs_source_set_monitoring_type() を呼ぶ対象
// (かつ compat-restore.json の保存対象)。
struct EnablePlan {
	std::vector<std::string> to_change;         // None → MONITOR_AND_OUTPUT へ変更する
	std::vector<std::string> already_ok;        // 既に MONITOR_AND_OUTPUT(変更不要)
	std::vector<std::string> warn_monitor_only; // MONITOR_ONLY のまま(制約 1、警告のみ)
	std::vector<std::string> self_silence_risk; // 事実 5 の警告対象(to_change/already_ok のサブセット)
};

inline EnablePlan BuildEnablePlan(const std::vector<SourceInfo> &sources, std::size_t mix_index,
				  const std::string &output_device_id, const std::string &default_device_id)
{
	EnablePlan plan;
	for (const auto &source : sources) {
		const SourceAction action = ClassifySource(source.mixers_bitmask, mix_index, source.monitoring_type);
		switch (action) {
		case SourceAction::NotApplicable:
			continue;
		case SourceAction::ChangeToMonitorAndOutput:
			plan.to_change.push_back(source.name);
			break;
		case SourceAction::AlreadyMonitorAndOutput:
			plan.already_ok.push_back(source.name);
			break;
		case SourceAction::WarnMonitorOnly:
			plan.warn_monitor_only.push_back(source.name);
			break;
		}
		if (action != SourceAction::WarnMonitorOnly &&
		    IsSelfMonitorSilenceRisk(source.has_do_not_self_monitor, output_device_id, default_device_id)) {
			plan.self_silence_risk.push_back(source.name);
		}
	}
	return plan;
}

// 復元プラン(制約 2)。saved は compat-restore.json の {ソース名, 元 monitoring type} 一覧
// (有効化時に to_change だけを保存したもの)。present_source_names は「現在実在が確認できたソース名」
// (monitor-compat-controller.cpp が obs_get_source_by_name() で確認済みのものだけを渡す)。
// 消えたソース(present_source_names に無い名前)は復元対象から除外する(REQ-COMPAT-001 制約 2:
// 「消えたソースはスキップ」)。
struct RestoreEntry {
	std::string name;
	MonitoringType original_type = MonitoringType::None;
};

inline std::vector<RestoreEntry> BuildRestorePlan(const std::vector<std::pair<std::string, MonitoringType>> &saved,
						  const std::vector<std::string> &present_source_names)
{
	std::vector<RestoreEntry> result;
	result.reserve(saved.size());
	for (const auto &entry : saved) {
		bool present = false;
		for (const auto &name : present_source_names) {
			if (name == entry.first) {
				present = true;
				break;
			}
		}
		if (present)
			result.push_back(RestoreEntry{entry.first, entry.second});
	}
	return result;
}

} // namespace compat_plan
