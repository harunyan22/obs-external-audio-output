// 対応 REQ: REQ-LOG-001, REQ-UI-008 / Slice 5
//
// src/diag/diagnostics-text.hpp(AC-22 の 15 項目の整形ロジック)の単体テスト。
// libobs/Windows API に依存しないため、プラグイン本体とは別にリンクして ctest から実行する
// (track-map.hpp/device-matcher.hpp と同方針)。

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../src/diag/diagnostics-text.hpp"

#include <algorithm>

using namespace diagnostics;

namespace {

DiagnosticsSnapshot SampleSnapshot()
{
	DiagnosticsSnapshot snapshot;
	snapshot.plugin_version = "0.1.0";
	snapshot.obs_version = "32.2.1";
	snapshot.os_version = "Windows 11 (10.0.26200)";
	snapshot.device_id = "{0.0.0.00000000}.{aaaa-bbbb}";
	snapshot.device_name = "Speakers (Realtek)";
	snapshot.obs_sample_rate = 48000;
	snapshot.output_sample_rate = 48000;
	snapshot.channel_count = 2;
	snapshot.buffer_chunks = 4;
	snapshot.buffer_chunks_requested = 3;
	snapshot.backend_name = "wasapi";
	snapshot.reconnect_events = 0;
	snapshot.underrun_count = 1;
	snapshot.overrun_count = 2;
	snapshot.clip_count = 3;
	snapshot.init_error = "none";
	snapshot.output_mode = "independent";
	snapshot.compat_apply_count = 0;
	snapshot.compat_restore_count = 0;
	snapshot.compat_reapply_count = 0;
	return snapshot;
}

} // namespace

TEST_CASE("BuildDiagnosticsText: AC-22 の 15 項目すべてがラベル付きで出力される")
{
	const std::string text = BuildDiagnosticsText(SampleSnapshot());

	CHECK(text.find("Plugin Version: 0.1.0") != std::string::npos);
	CHECK(text.find("OBS Version: 32.2.1") != std::string::npos);
	CHECK(text.find("OS Version: Windows 11 (10.0.26200)") != std::string::npos);
	CHECK(text.find("Selected Device ID: {0.0.0.00000000}.{aaaa-bbbb}") != std::string::npos);
	CHECK(text.find("Selected Device Name: Speakers (Realtek)") != std::string::npos);
	CHECK(text.find("OBS Sample Rate: 48000 Hz") != std::string::npos);
	CHECK(text.find("Output Sample Rate: 48000 Hz") != std::string::npos);
	CHECK(text.find("Channel Count: 2") != std::string::npos);
	CHECK(text.find("Buffer Size: 4 chunks") != std::string::npos);
	// レビュー指摘 M-9: 要求値(切り上げ前)も併記される。
	CHECK(text.find("Buffer Size: 4 chunks (requested 3)") != std::string::npos);
	CHECK(text.find("Audio Backend: wasapi") != std::string::npos);
	CHECK(text.find("Device Reconnect Events: 0") != std::string::npos);
	CHECK(text.find("Buffer Underrun Count: 1") != std::string::npos);
	CHECK(text.find("Buffer Overrun Count: 2") != std::string::npos);
	CHECK(text.find("Clip Count: 3") != std::string::npos);
	CHECK(text.find("Initialization Errors: none") != std::string::npos);

	// Slice 9(REQ-COMPAT-001): 15 項目の後ろに追記される互換モード情報。
	CHECK(text.find("Output Mode: independent") != std::string::npos);
	CHECK(text.find("Compat Apply Count: 0") != std::string::npos);
	CHECK(text.find("Compat Restore Count: 0") != std::string::npos);
	CHECK(text.find("Compat Reapply Count: 0") != std::string::npos);
}

TEST_CASE("BuildDiagnosticsText: 互換モードのイベントカウンタがそのまま表示される")
{
	DiagnosticsSnapshot snapshot = SampleSnapshot();
	snapshot.output_mode = "obs_monitor_compat";
	snapshot.compat_apply_count = 2;
	snapshot.compat_restore_count = 1;
	snapshot.compat_reapply_count = 5;

	const std::string text = BuildDiagnosticsText(snapshot);

	CHECK(text.find("Output Mode: obs_monitor_compat") != std::string::npos);
	CHECK(text.find("Compat Apply Count: 2") != std::string::npos);
	CHECK(text.find("Compat Restore Count: 1") != std::string::npos);
	CHECK(text.find("Compat Reapply Count: 5") != std::string::npos);
}

TEST_CASE("BuildDiagnosticsText: 空文字列のフィールドは (unknown) と表示される")
{
	DiagnosticsSnapshot snapshot; // 全フィールド既定値(文字列は空、数値は 0)。

	const std::string text = BuildDiagnosticsText(snapshot);

	CHECK(text.find("Plugin Version: (unknown)") != std::string::npos);
	CHECK(text.find("Selected Device ID: (unknown)") != std::string::npos);
	CHECK(text.find("Selected Device Name: (unknown)") != std::string::npos);
	CHECK(text.find("Audio Backend: (unknown)") != std::string::npos);
	// init_error が空文字列のときは "none" にフォールバックする("(unknown)" にはしない:
	// 「未設定」と「エラー無し」は概念上区別する)。
	CHECK(text.find("Initialization Errors: none") != std::string::npos);
}

TEST_CASE("BuildDiagnosticsText: 初期化エラーがあればそのまま表示される")
{
	DiagnosticsSnapshot snapshot = SampleSnapshot();
	snapshot.init_error = "WASAPI Open failed (see log for hr)";

	const std::string text = BuildDiagnosticsText(snapshot);

	CHECK(text.find("Initialization Errors: WASAPI Open failed (see log for hr)") != std::string::npos);
	// "none" という部分文字列自体は上の文言に含まれない(誤検知しないことの確認)。
	CHECK(text.find("Initialization Errors: none") == std::string::npos);
}

TEST_CASE(
	"BuildDiagnosticsText: AC-22 の 15 項目 + Slice 9 の互換モード追加 4 項目が 1 行 1 項目で改行区切りになっている")
{
	const std::string text = BuildDiagnosticsText(SampleSnapshot());

	// AC-22 の 15 項目 + Output Mode/Compat Apply/Restore/Reapply Count の 4 項目(Slice 9、既存
	// 15 項目の後ろに追記。既存 15 項目の件数・順序は変えない)。
	const long long line_count = std::count(text.begin(), text.end(), '\n') + 1;
	CHECK(line_count == 19);
}
