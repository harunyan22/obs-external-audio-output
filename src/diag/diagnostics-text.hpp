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

// 対応 REQ: REQ-LOG-001, REQ-UI-008 / Slice 5
//
// AC-22 の 15 項目(Plugin/OBS/OS Version、Selected Device ID/Name、OBS/Output Sample Rate、
// Channel Count、Buffer Size、Audio backend、Device reconnect events、Buffer underrun/overrun
// count、Clip count、Initialization errors)を保持するスナップショットと、その整形テキストを
// 生成する純粋関数。OBS/Windows API に一切依存しないため tests/unit/test-diagnostics-text.cpp から
// libobs 抜きで単体テストできる(track-map.hpp/device-matcher.hpp と同方針)。
//
// 診断情報コピー(REQ-UI-008、AC-25、Dock の「診断情報をコピー」ボタン)と、開始/停止時の OBS ログ
// 出力(REQ-LOG-001、diag/diagnostics.cpp の LogDiagnosticsSnapshot())の両方がこの
// BuildDiagnosticsText() を共有することで、コピー用と blog 用の整形ロジックを 1 箇所に集約する
// (ブリーフ実装内容: 診断テキスト生成の共通化)。

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace diagnostics {

// AC-22 の 15 項目に対応するフィールドを持つスナップショット。値の取得元(OutputInstance)は
// 別ファイル(output-instance.cpp)の責務で、このヘッダは整形のみを担う。
struct DiagnosticsSnapshot {
	std::string plugin_version;
	std::string obs_version;
	std::string os_version;
	std::string device_id;
	std::string device_name;
	uint32_t obs_sample_rate = 0;
	uint32_t output_sample_rate = 0;
	uint16_t channel_count = 0;
	// リングバッファの容量(チャンク数、2 の冪へ切り上げ後の実容量)。REQUIREMENTS.md §3 の
	// "Buffer Size" 相当(M-09)。
	uint32_t buffer_chunks = 0;
	// レビュー指摘 M-9: buffer_preset/buffer_chunks(Advanced 設定)から要求されたチャンク数
	// (切り上げ前)。RingBuffer は内部で 2 の冪へ切り上げるため、buffer_chunks(実容量)だけでは
	// 「low_latency=3 を指定したのに表示は 4」のようなズレが分かりにくい。両方を出す。
	uint32_t buffer_chunks_requested = 0;
	std::string backend_name;
	// S6(再接続実装)までは常に 0(REQ-DEVICE-002/003 は未実装)。
	uint64_t reconnect_events = 0;
	uint64_t underrun_count = 0;
	uint64_t overrun_count = 0;
	uint64_t clip_count = 0;
	// 直近セッションの最後の初期化失敗を短く説明する文字列。エラーが無ければ "none"。
	std::string init_error;

	// Slice 9(REQ-COMPAT-001): AC-22 の 15 項目とは別枠の追加情報(既存 15 項目の意味・順序・件数は
	// 変えない。BuildDiagnosticsText() がこれらを 15 項目の後ろに追記する)。
	// "independent" / "obs_monitor_compat"(plugin_config::PluginConfig::output_mode と同じ値域)。
	std::string output_mode = "independent";
	// MonitorCompatController の適用/復元/再適用回数(プロセス起動からの累積)。
	uint64_t compat_apply_count = 0;
	uint64_t compat_restore_count = 0;
	uint64_t compat_reapply_count = 0;
};

namespace detail {

inline const std::string &OrUnknown(const std::string &value)
{
	static const std::string kUnknown = "(unknown)";
	return value.empty() ? kUnknown : value;
}

} // namespace detail

// スナップショットを 1 項目 1 行のテキストへ整形する(コピー用/ログ用共通)。改行は "\n" のみ
// (呼び出し元が obs_log/QClipboard それぞれに適した形でそのまま使える)。
inline std::string BuildDiagnosticsText(const DiagnosticsSnapshot &snapshot)
{
	char line[192];
	std::string out;
	out.reserve(512);

	out += "Plugin Version: " + detail::OrUnknown(snapshot.plugin_version) + "\n";
	out += "OBS Version: " + detail::OrUnknown(snapshot.obs_version) + "\n";
	out += "OS Version: " + detail::OrUnknown(snapshot.os_version) + "\n";
	out += "Selected Device ID: " + detail::OrUnknown(snapshot.device_id) + "\n";
	out += "Selected Device Name: " + detail::OrUnknown(snapshot.device_name) + "\n";

	snprintf(line, sizeof(line), "OBS Sample Rate: %u Hz", (unsigned)snapshot.obs_sample_rate);
	out += line;
	out += "\n";
	snprintf(line, sizeof(line), "Output Sample Rate: %u Hz", (unsigned)snapshot.output_sample_rate);
	out += line;
	out += "\n";
	snprintf(line, sizeof(line), "Channel Count: %u", (unsigned)snapshot.channel_count);
	out += line;
	out += "\n";
	// レビュー指摘 M-9: 要求値(切り上げ前)と実容量(2 の冪切り上げ後)の両方を出す
	// (例: buffer_preset=low_latency で 3 チャンクを要求しても実容量は 4 になる)。
	snprintf(line, sizeof(line), "Buffer Size: %u chunks (requested %u)", (unsigned)snapshot.buffer_chunks,
		 (unsigned)snapshot.buffer_chunks_requested);
	out += line;
	out += "\n";

	out += "Audio Backend: " + detail::OrUnknown(snapshot.backend_name) + "\n";

	snprintf(line, sizeof(line), "Device Reconnect Events: %llu", (unsigned long long)snapshot.reconnect_events);
	out += line;
	out += "\n";
	snprintf(line, sizeof(line), "Buffer Underrun Count: %llu", (unsigned long long)snapshot.underrun_count);
	out += line;
	out += "\n";
	snprintf(line, sizeof(line), "Buffer Overrun Count: %llu", (unsigned long long)snapshot.overrun_count);
	out += line;
	out += "\n";
	snprintf(line, sizeof(line), "Clip Count: %llu", (unsigned long long)snapshot.clip_count);
	out += line;
	out += "\n";

	out += "Initialization Errors: ";
	out += snapshot.init_error.empty() ? std::string("none") : snapshot.init_error;
	out += "\n";

	// Slice 9: AC-22 の 15 項目の後ろに追記する追加情報(既存 15 項目の件数・順序は変えない)。
	out += "Output Mode: " + detail::OrUnknown(snapshot.output_mode) + "\n";
	snprintf(line, sizeof(line), "Compat Apply Count: %llu", (unsigned long long)snapshot.compat_apply_count);
	out += line;
	out += "\n";
	snprintf(line, sizeof(line), "Compat Restore Count: %llu", (unsigned long long)snapshot.compat_restore_count);
	out += line;
	out += "\n";
	snprintf(line, sizeof(line), "Compat Reapply Count: %llu", (unsigned long long)snapshot.compat_reapply_count);
	out += line;

	return out;
}

} // namespace diagnostics
