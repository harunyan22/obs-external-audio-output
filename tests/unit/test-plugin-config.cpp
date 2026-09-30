// 対応 REQ: REQ-CONFIG-001, REQ-CONFIG-002 / Slice 7
//
// src/config/plugin-config.hpp(値域クランプ・schema_version 判定・§3 既定値)の単体テスト。
// Load()/Save()(obs_data 依存)は plugin-config.cpp 側にあり libobs なしでは呼べないため、ここでは
// ヘッダに inline 定義された純粋関数と PluginConfig の既定値のみを対象にする(track-map.hpp と
// 同じパターン)。libobs に依存しないため、プラグイン本体とは別にリンクして ctest から実行する。
//
// レビュー指摘 Low-8(正直化): 以下の「ラウンドトリップ」テストは ClampBufferChunks() と
// IsSupportedSchemaVersion() の 2 つの純粋関数のみを検証する。実際の Load()/Save()
// (obs_data_create_from_json_file_safe/obs_data_save_json_safe を介した実ファイル I/O)は
// libobs 依存でこのテストバイナリからは呼べないため、ここでは検証していない(未検証)。
// 「保存 → 読込」の意味論そのものを実ファイル経由で確認したい場合は、実機/統合テストで別途
// 確認すること。

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../src/config/plugin-config.hpp"

using namespace plugin_config;

TEST_CASE("ClampBufferChunks: 範囲内(2〜16)はそのまま")
{
	CHECK(ClampBufferChunks(2) == 2);
	CHECK(ClampBufferChunks(16) == 16);
	CHECK(ClampBufferChunks(4) == 4);
}

TEST_CASE("ClampBufferChunks: 範囲外は 2〜16 にクランプ")
{
	CHECK(ClampBufferChunks(0) == 2);
	CHECK(ClampBufferChunks(1) == 2);
	CHECK(ClampBufferChunks(17) == 16);
	CHECK(ClampBufferChunks(1000) == 16);
}

TEST_CASE("IsSupportedSchemaVersion: kSchemaVersion 以下は true")
{
	CHECK(IsSupportedSchemaVersion(1));
	CHECK(IsSupportedSchemaVersion(0));
	CHECK(IsSupportedSchemaVersion(-1));
	CHECK(kSchemaVersion == 1);
}

TEST_CASE("IsSupportedSchemaVersion: kSchemaVersion より大きい値は false(将来の破壊的変更)")
{
	CHECK_FALSE(IsSupportedSchemaVersion(2));
	CHECK_FALSE(IsSupportedSchemaVersion(100));
}

TEST_CASE("PluginConfig の既定値は REQUIREMENTS.md §3 と一致する")
{
	PluginConfig config;
	CHECK(config.schema_version == 1);
	CHECK(config.enabled == true);
	CHECK(config.audio_track == 6);
	CHECK(config.device_id == "default");
	CHECK(config.device_name == "");
	CHECK(config.buffer_preset == "auto");
	CHECK(config.buffer_chunks == 4);
	CHECK(config.auto_reconnect == true);
	CHECK(config.log_level == "info");
	CHECK(config.backend == "wasapi");
	CHECK(config.output_mode == "independent");
}

// Slice 8 レビュー Low-8(正直化): 旧テストは「Save() が書き出す値」を手動でコピーしただけの
// round_tripped を作り、その多くのフィールド(device_id/device_name/buffer_preset/log_level 等)を
// 「コピーした値と元の値が一致する」というトートロジーとして CHECK していた(plugin-config.cpp の
// Save()/Load() を実際には一切呼んでいないため、これらのフィールドは何も検証していなかった)。
// 以下は、Load()/Save() が実際に適用する非自明な変換(値域クランプ ClampBufferChunks() と
// schema_version 判定 IsSupportedSchemaVersion())だけを対象にする。ファイル I/O を伴う実際の
// Load()→Save()→Load() ラウンドトリップは対象外のまま(冒頭コメント参照、未検証)。

TEST_CASE("buffer_chunks: 域内の値は ClampBufferChunks() で変化しない(Load() 相当)")
{
	CHECK(ClampBufferChunks(10) == 10); // custom buffer_chunks=10 は 2〜16 の域内。
}

TEST_CASE("buffer_chunks: 域外の値は Load() 相当で 2〜16 にクランプされる")
{
	// Save() 自体は clamp しない(plugin-config.cpp の Save() は config.buffer_chunks をそのまま
	// obs_data_set_int する)。クランプは Load() 側でのみ行われる(plugin-config.cpp の
	// ClampBufferChunks(unclamped) 呼び出し)。ここではその非対称性(書き込みは無条件、
	// 読み込みでクランプ)を明示的に検証する。
	CHECK(ClampBufferChunks(0) == 2);
	CHECK(ClampBufferChunks(1000) == 16);
}

TEST_CASE("schema_version: kSchemaVersion 以下なら Load() はファイルの値をそのまま使ってよい")
{
	CHECK(IsSupportedSchemaVersion(kSchemaVersion));
}

TEST_CASE("schema_version: 未来値なら Load() は既定値一式へフォールバックする")
{
	// plugin-config.cpp の Load(): IsSupportedSchemaVersion(schema_version) が false なら、
	// ファイルの内容を一切使わず PluginConfig の既定値一式を返す(他フィールドの値に関わらず)。
	const long long future_schema_version = kSchemaVersion + 1;
	CHECK_FALSE(IsSupportedSchemaVersion(future_schema_version));

	// この場合 Load() が返す想定の値は「保存されていた値」ではなく既定値そのもの。
	PluginConfig fallback_expected;
	CHECK(fallback_expected.schema_version == kSchemaVersion);
	CHECK(fallback_expected.audio_track == audio_track_map::kDefaultAudioTrack);
}

// 2026-09-01 配布準備改定(REQUIREMENTS.md §3 enabled 行、ユーザー承認)。plugin-config.cpp の
// Load(): config.json が存在しない(os_file_exists(path)==false)ときだけ config.enabled=false に
// 上書きして Save(config) で初回ファイルを生成する。obs_data 経由の実ファイル I/O
// (os_file_exists/obs_data_create_from_json_file_safe/Save())はこのテストバイナリからは呼べない
// ため、ここでは「PluginConfig の既定値から enabled だけ false にしたものが初回起動時の期待値」
// であることを構造的に確認するのみ(未検証。実ファイル I/O 経由の確認は実機/統合テストで別途行う)。
TEST_CASE("初回起動(config.json 不在)想定値: enabled のみ既定値から false に変わる")
{
	PluginConfig default_config;
	CHECK(default_config.enabled == true);

	PluginConfig first_run_expected;
	first_run_expected.enabled = false;

	CHECK(first_run_expected.enabled == false);
	CHECK(first_run_expected.audio_track == default_config.audio_track);
	CHECK(first_run_expected.device_id == default_config.device_id);
	CHECK(first_run_expected.device_name == default_config.device_name);
	CHECK(first_run_expected.buffer_preset == default_config.buffer_preset);
	CHECK(first_run_expected.buffer_chunks == default_config.buffer_chunks);
	CHECK(first_run_expected.auto_reconnect == default_config.auto_reconnect);
	CHECK(first_run_expected.log_level == default_config.log_level);
	CHECK(first_run_expected.backend == default_config.backend);
	CHECK(first_run_expected.output_mode == default_config.output_mode);
}
