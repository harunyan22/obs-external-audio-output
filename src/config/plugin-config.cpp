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

#include "plugin-config.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <cstring>

namespace plugin_config {

// ClampBufferChunks()/IsSupportedSchemaVersion() は plugin-config.hpp で inline 定義済み
// (tests/unit/test-plugin-config.cpp が libobs 抜きでリンクできるようにするため)。

PluginConfig Load()
{
	PluginConfig config;

	char *path = obs_module_config_path("config.json");
	if (!path) {
		obs_log(LOG_WARNING, "plugin_config::Load: obs_module_config_path failed; using defaults");
		return config;
	}

	const bool config_file_exists = os_file_exists(path);
	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);

	if (!data) {
		if (config_file_exists) {
			obs_log(LOG_WARNING,
				"plugin_config::Load: config.json exists but failed to parse; using defaults");
		} else {
			// 2026-09-01 配布準備改定(ユーザー承認、REQUIREMENTS.md §3 enabled 行): config.json が
			// 存在しない初回起動は、インストール直後に音が出る「驚き」を防ぐため enabled=false で
			// 生成・保存する。既存ファイルがある場合(config_file_exists==true、パース失敗を含む)は
			// この分岐に来ないため、§3 既定の enabled=true(従来挙動)のまま。
			config.enabled = false;
			obs_log(LOG_INFO,
				"plugin_config::Load: config.json not found; creating with enabled=false (first run)");
			Save(config);
		}
		return config;
	}

	// D-13: schema_version がこのビルドより新しければ、他のキーの意味づけが変わっている可能性が
	// あるため一切信用せず既定値一式で継続する(ブリーフ: 「>1 は警告して既定継続」)。キーが無い
	// (旧ファイル/初回)場合は kSchemaVersion(1)相当として通常どおり読む。
	if (obs_data_has_user_value(data, "schema_version")) {
		const long long schema_version = obs_data_get_int(data, "schema_version");
		if (!IsSupportedSchemaVersion(schema_version)) {
			obs_log(LOG_WARNING,
				"plugin_config::Load: config.json schema_version=%lld is newer than supported "
				"(%d); ignoring file contents and using defaults",
				schema_version, kSchemaVersion);
			obs_data_release(data);
			return config;
		}
	}

	if (obs_data_has_user_value(data, "enabled"))
		config.enabled = obs_data_get_bool(data, "enabled");

	if (obs_data_has_user_value(data, "audio_track")) {
		const long long audio_track_raw = obs_data_get_int(data, "audio_track");
		bool clamped = false;
		config.audio_track = audio_track_map::ClampAudioTrack((int)audio_track_raw, &clamped);
		if (clamped) {
			obs_log(LOG_WARNING,
				"plugin_config::Load: config.json audio_track=%lld is out of range (%d-%d); "
				"clamped",
				audio_track_raw, audio_track_map::kMinAudioTrack, audio_track_map::kMaxAudioTrack);
		}
	}

	if (obs_data_has_user_value(data, "device_id")) {
		const char *device_id = obs_data_get_string(data, "device_id");
		if (device_id && device_id[0] != '\0')
			config.device_id = device_id;
	}
	if (obs_data_has_user_value(data, "device_name")) {
		const char *device_name = obs_data_get_string(data, "device_name");
		if (device_name)
			config.device_name = device_name;
	}

	if (obs_data_has_user_value(data, "buffer_preset")) {
		const char *buffer_preset = obs_data_get_string(data, "buffer_preset");
		if (buffer_preset && buffer_preset[0] != '\0')
			config.buffer_preset = buffer_preset;
	}
	if (obs_data_has_user_value(data, "buffer_chunks")) {
		const long long buffer_chunks_raw = obs_data_get_int(data, "buffer_chunks");
		const uint32_t unclamped = buffer_chunks_raw < 0 ? 0 : (uint32_t)buffer_chunks_raw;
		config.buffer_chunks = ClampBufferChunks(unclamped);
	}
	if (obs_data_has_user_value(data, "auto_reconnect"))
		config.auto_reconnect = obs_data_get_bool(data, "auto_reconnect");
	if (obs_data_has_user_value(data, "log_level")) {
		const char *log_level = obs_data_get_string(data, "log_level");
		if (log_level && log_level[0] != '\0')
			config.log_level = log_level;
	}

	// backend: MVP では固定値以外を無視する(§3: backend="wasapi" 固定)。
	if (obs_data_has_user_value(data, "backend")) {
		const char *backend = obs_data_get_string(data, "backend");
		if (backend && std::strcmp(backend, "wasapi") == 0)
			config.backend = backend;
	}
	// output_mode(Slice 9、§3): "independent" / "obs_monitor_compat" の 2 列挙。未知値は
	// independent へフォールバックし警告する(slice-9.md 実装変更範囲)。
	if (obs_data_has_user_value(data, "output_mode")) {
		const char *output_mode = obs_data_get_string(data, "output_mode");
		if (output_mode && (std::strcmp(output_mode, "independent") == 0 ||
				    std::strcmp(output_mode, "obs_monitor_compat") == 0)) {
			config.output_mode = output_mode;
		} else if (output_mode && output_mode[0] != '\0') {
			obs_log(LOG_WARNING,
				"plugin_config::Load: config.json output_mode=\"%s\" is not a known value; falling "
				"back to \"independent\"",
				output_mode);
		}
	}

	obs_data_release(data);
	return config;
}

bool Save(const PluginConfig &config)
{
	char *path = obs_module_config_path("config.json");
	if (!path) {
		obs_log(LOG_ERROR, "plugin_config::Save: obs_module_config_path failed; config not saved");
		return false;
	}

	// 旧#17/D-13: obs_module_config_path はディレクトリを自動生成しないため、書き込み前に毎回
	// os_mkdirs する(text-freetype2/rtmp-services 等の同梱プラグインと同じパターン。os_mkdirs は
	// 既存ディレクトリに対しても安全な冪等呼び出し)。
	char *dir = obs_module_config_path(nullptr);
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "schema_version", kSchemaVersion);
	obs_data_set_bool(data, "enabled", config.enabled);
	obs_data_set_int(data, "audio_track", config.audio_track);
	obs_data_set_string(data, "device_id", config.device_id.c_str());
	obs_data_set_string(data, "device_name", config.device_name.c_str());
	obs_data_set_string(data, "buffer_preset", config.buffer_preset.c_str());
	obs_data_set_int(data, "buffer_chunks", config.buffer_chunks);
	obs_data_set_bool(data, "auto_reconnect", config.auto_reconnect);
	obs_data_set_string(data, "log_level", config.log_level.c_str());
	obs_data_set_string(data, "backend", config.backend.c_str());
	obs_data_set_string(data, "output_mode", config.output_mode.c_str());

	const bool ok = obs_data_save_json_safe(data, path, "tmp", "bak");
	if (!ok) {
		obs_log(LOG_ERROR, "plugin_config::Save: obs_data_save_json_safe failed (path=%s)", path);
	}

	obs_data_release(data);
	bfree(path);
	return ok;
}

} // namespace plugin_config
