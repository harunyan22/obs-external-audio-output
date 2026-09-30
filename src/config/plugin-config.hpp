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

// 対応 REQ: REQ-CONFIG-001, REQ-CONFIG-002 / Slice 7
//
// config.json(REQUIREMENTS.md §3、正本)の読み書きをこの 1 モジュールへ集約する(D-13)。
// 値域クランプ・schema_version 判定は obs_data に依存しない純粋関数として切り出し、
// tests/unit/test-plugin-config.cpp から libobs 抜きでテストできるようにする(track-map.hpp の
// 既存パターンを踏襲)。obs_data を実際に読み書きする Load()/Save() は plugin-config.cpp
// (libobs 依存)で実装する。

#pragma once

#include "../audio/track-map.hpp"

#include <cstdint>
#include <string>

namespace plugin_config {

// 現在サポートするスキーマバージョン(D-13)。ファイルの schema_version がこれより大きい場合は
// 「将来の破壊的変更」とみなし、Load() はファイルの内容を信用せず既定値一式で継続する。
constexpr int kSchemaVersion = 1;

constexpr uint32_t kMinBufferChunks = 2;
constexpr uint32_t kMaxBufferChunks = 16;

// REQUIREMENTS.md §3 のスキーマそのもの(JSON 例と 1:1)。
struct PluginConfig {
	int schema_version = kSchemaVersion;
	// ユーザーの ON 意思の永続化(D-11、AC-08 の根拠)。§3 の既定値どおり true(ブリーフ実装内容 5:
	// device_id 既定 "default" と合わせて、初回起動時は既定デバイスから音が出る仕様どおりの挙動)。
	bool enabled = true;
	int audio_track = audio_track_map::kDefaultAudioTrack;
	std::string device_id = "default";
	std::string device_name;
	// "auto" / "low_latency" / "stable" / "custom"。
	std::string buffer_preset = "auto";
	// buffer_preset=="custom" のときのみ有効(2〜16)。
	uint32_t buffer_chunks = 4;
	bool auto_reconnect = true;
	// "error" / "warning" / "info" / "debug"。
	std::string log_level = "info";
	// MVP では "wasapi" 固定(§3)。
	std::string backend = "wasapi";
	// "independent"(既定)/ "obs_monitor_compat"(互換モード、Slice 9、REQ-COMPAT-001)。両モードは
	// 排他(D-04)。未知値は Load() が "independent" へフォールバックする。
	std::string output_mode = "independent";
};

// buffer_chunks を 2〜16 にクランプする(§3)。output-instance.cpp の
// OutputInstance::ResolveBufferChunks() の custom 分岐と Load() の両方から使う共通実装。
// track-map.hpp の ClampAudioTrack() と同じ理由(tests/unit/test-plugin-config.cpp から libobs
// 抜きでリンクできるようにするため)で inline 定義にする。
inline uint32_t ClampBufferChunks(uint32_t chunks)
{
	if (chunks < kMinBufferChunks)
		return kMinBufferChunks;
	if (chunks > kMaxBufferChunks)
		return kMaxBufferChunks;
	return chunks;
}

// config.json の schema_version がこのビルドで読める範囲かを判定する(D-13)。現状は
// 「kSchemaVersion 以下なら true」。ClampBufferChunks() と同じ理由で inline 定義。
inline bool IsSupportedSchemaVersion(long long schema_version)
{
	return schema_version <= kSchemaVersion;
}

// config.json を読む(obs_module_config_path("config.json") +
// obs_data_create_from_json_file_safe)。ファイル不在/破損はすべて既定値へフォールバックする。
// schema_version がこのビルドの kSchemaVersion より大きい場合は警告ログを出し、ファイルの内容を
// 一切使わず既定値一式を返す(ブリーフ: 「schema_version 検査(>1 は警告して既定継続)」)。
// キー単位の欠落はそのキーだけ既定値のまま(既存 LoadConfig の挙動を踏襲)。UI/control thread の
// どちらから呼んでも安全(ファイル I/O のみ、RT では呼ばない)。
PluginConfig Load();

// config.json へ書く(os_mkdirs で親ディレクトリを作成 → obs_data_save_json_safe(data, path,
// "tmp", "bak"))。§3 のキーのみを書く(未知キー保持はしない)。成功したら true。呼び出しは
// control thread または UI スレッドから(RT では呼ばない)。
bool Save(const PluginConfig &config);

} // namespace plugin_config
