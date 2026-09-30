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

// 対応 REQ: REQ-DEVICE-001 / Slice 4
//
// 保存済み {device_id, device_name}(config.json)を、現在アクティブな出力デバイスの列挙結果から
// 解決する純粋ロジック(D-14)。libobs/COM/Windows API に一切依存しない(単体テスト対象、
// tests/unit/test-device-matcher.cpp から libobs 抜きでビルド・実行できる)。
//
// 解決順序: id 一致 → 名前一致(このとき「新しい id」= 列挙結果側の id を採用して返す) → 未解決。
// 未解決時は std::nullopt を返す(自動フォールバック再生はしない。呼び出し元が Failed へ遷移する)。
// saved_device_id == "default" は常に無条件で解決成功(ResolvedBy::Default)を返す
// (実際に既定デバイスを取得できるかどうかは AudioDeviceManager/WasapiOutput 側の責務であり、
// このロジックの関知するところではない)。

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace device_matcher {

// 予約値。config.json の device_id がこの文字列なら OS 既定デバイス(role eConsole)を指す(D-14)。
constexpr const char *kDefaultDeviceId = "default";

// 列挙されたデバイス 1 件(id + friendly name)。AudioDeviceManager の列挙結果をそのまま渡せる形。
struct DeviceEntry {
	std::string id;
	std::string name;
};

enum class ResolvedBy {
	Default, // saved_device_id == "default"
	ById,    // id が列挙リストと一致
	ByName,  // id は不一致だが name が一致(新しい id を採用)
};

// Resolve() が std::nullopt を返す理由(レビュー指摘 M-3)。呼び出し元(OutputInstance)がログで
// 「一致なし」と「名前が曖昧で選べない」を区別するために使う。out_reason に nullptr を渡せば無視できる。
enum class UnresolvedReason {
	NoMatch,       // id・name のいずれにも一致するデバイスがない
	AmbiguousName, // saved_device_name に一致する候補が 2 件以上あり、どれを採用すべきか決定できない
};

struct ResolveResult {
	// ResolvedBy::Default のときは "default" 固定。それ以外は列挙リスト側の id(ByName のときも
	// 「新しい id」)。
	std::string id;
	// ResolvedBy::Default のときは呼び出し元が別途(AudioDeviceManager の既定デバイス取得結果から)
	// 埋めること。このロジックは既定デバイスの実名を知らない。
	std::string name;
	ResolvedBy resolved_by = ResolvedBy::ById;
};

// 保存済み {saved_device_id, saved_device_name} を devices から解決する。devices が空でも
// saved_device_id == "default" なら解決成功として扱う。
// out_reason/out_ambiguous_count(レビュー指摘 M-3): 未解決(nullopt)のとき、呼び出し元がログに
// 出す理由の補足として書き込む。out_ambiguous_count は out_reason が AmbiguousName のときのみ
// 意味を持ち、名前が一致した候補の件数(2 件以上)を表す。どちらも nullptr を渡せば無視される。
inline std::optional<ResolveResult> Resolve(const std::string &saved_device_id, const std::string &saved_device_name,
					    const std::vector<DeviceEntry> &devices,
					    UnresolvedReason *out_reason = nullptr,
					    std::size_t *out_ambiguous_count = nullptr)
{
	if (saved_device_id == kDefaultDeviceId)
		return ResolveResult{kDefaultDeviceId, "", ResolvedBy::Default};

	for (const auto &entry : devices) {
		if (entry.id == saved_device_id)
			return ResolveResult{entry.id, entry.name, ResolvedBy::ById};
	}

	if (!saved_device_name.empty()) {
		const DeviceEntry *match = nullptr;
		std::size_t match_count = 0;
		for (const auto &entry : devices) {
			if (entry.name == saved_device_name) {
				++match_count;
				if (!match)
					match = &entry;
			}
		}
		if (match_count == 1)
			return ResolveResult{match->id, match->name, ResolvedBy::ByName};
		if (match_count > 1) {
			// 同名デバイスが複数あると「新しい id」を一意に決められないため、名前一致でも
			// 未解決扱いにする(誤った id を採用しないほうが安全側。レビュー指摘 M-3)。
			if (out_reason)
				*out_reason = UnresolvedReason::AmbiguousName;
			if (out_ambiguous_count)
				*out_ambiguous_count = match_count;
			return std::nullopt;
		}
	}

	if (out_reason)
		*out_reason = UnresolvedReason::NoMatch;
	return std::nullopt;
}

} // namespace device_matcher
