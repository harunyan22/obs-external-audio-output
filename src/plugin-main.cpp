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

// 対応 REQ: REQ-CORE-001 / Slice 0, REQ-AUDIO-001 / Slice 1, REQ-AUDIO-002, REQ-UI-002,
// REQ-LIFECYCLE-001 / Slice 2, REQ-DEVICE-001 / Slice 4, REQ-UI-001, REQ-UI-004〜008 / Slice 5,
// REQ-AUDIO-007, REQ-DEVICE-002〜005, REQ-STABILITY-001 / Slice 6, REQ-CONFIG-001, REQ-CONFIG-002 /
// Slice 7

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "compat/monitor-compat-controller.hpp"
#include "config/plugin-config.hpp"
#include "device/audio-device-manager.hpp"
#include "output/output-instance.hpp"
#include "ui/external-output-dock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

const char *kDockId = "obs-external-audio-output-dock";

// Slice 7: FINISHED_LOADING で enabled=true かつ device_id があれば自動開始する
// (REQ-CONFIG-002、AC-21、qt-ui-and-lifecycle.md)。EXIT で完全停止する(同ファイル)。
OutputInstance g_output_instance;

// Slice 5: Dock 本体(QWidget、所有権は OBS。delete しない、qt-ui-and-lifecycle.md)。EXIT 時に
// QTimer を止めるためだけに生ポインタで保持する。obs_module_load() で構築後、以後プロセス終了まで
// 生存する前提(Dock ウィジェットは OBS が破棄するまで存在し、g_output_instance と同じ寿命)。
ExternalOutputDock *g_dock = nullptr;

void OnFrontendEvent(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);

	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		// Slice 6(ブリーフ実装内容 1): control thread をここで起動する。
		g_output_instance.StartControlThread();

		// Slice 7(REQ-CONFIG-002、AC-21、qt-ui-and-lifecycle.md「enabled=true かつ device_id が
		// あれば自動開始」): 起動直後の control thread へ Start コマンドを enqueue するだけ
		// (即 return、フロントエンドイベントスレッドをブロックしない)。実際の接続試行は
		// HandleStartCommand() が config.json を読み直して行う(ここで読んだ値は enabled/device_id
		// の判定にのみ使う、二重読み込みにはなるが FINISHED_LOADING は 1 回しか発生しない)。
		const plugin_config::PluginConfig config = plugin_config::Load();
		// Slice 9(REQ-COMPAT-001): output_mode="obs_monitor_compat" のときは独立出力を自動開始しない
		// (HandleStartCommand() 側でもガードされるが、ここで先に分岐して意図をログに残す。実際の
		// compat 自動適用/クラッシュ耐性復元は g_dock->ApplyStartupCompatState() が行う)。
		if (config.output_mode == "obs_monitor_compat") {
			obs_log(LOG_DEBUG, "plugin-main: independent auto-start skipped at FINISHED_LOADING "
					   "(output_mode=\"obs_monitor_compat\")");
		} else if (config.enabled && !config.device_id.empty()) {
			// Slice 8(S7 持ち越しの衛生、追跡性): 自動開始の意思決定を明示ログに残す
			// (enabled/device_id のどちらが原因で開始しなかったかを後から追えるようにする)。
			obs_log(LOG_INFO,
				"plugin-main: auto-start requested at FINISHED_LOADING (enabled=true device_id=\"%s\")",
				config.device_id.c_str());
			g_output_instance.Start();
		} else {
			// レビュー指摘 Low-10: 未設定/無効の通常運用でも毎起動 1 回出る行のため、自動開始した
			// 場合の LOG_INFO とは非対称に LOG_DEBUG にする(「何もしなかった」ことの記録であり、
			// 通常運用でログを埋めるほどの重要度ではない)。
			obs_log(LOG_DEBUG,
				"plugin-main: auto-start skipped at FINISHED_LOADING (enabled=%s device_id=\"%s\")",
				config.enabled ? "true" : "false", config.device_id.c_str());
		}

		// Slice 9(REQ-COMPAT-001): output_mode="obs_monitor_compat" && enabled なら互換モードを
		// 自動適用し、それ以外では compat-restore.json の残骸を起動時復元する(クラッシュ耐性、S9-7)。
		if (g_dock)
			g_dock->ApplyStartupCompatState();
		return;
	}

	if (event == OBS_FRONTEND_EVENT_PROFILE_CHANGED) {
		// D-06(obs_reset_audio 追従、ブリーフ実装内容 10): raw callback の再登録を control thread へ
		// enqueue するだけ(即 return、フロントエンドイベントスレッドをブロックしない)。
		g_output_instance.RequestReregister();
		// Slice 9(REQ-COMPAT-001 制約3、S9-3): LoadProfile() がモニタリングデバイスを basic.ini の
		// 値へ上書きするため、互換モード有効中は再適用する。
		if (g_dock)
			g_dock->OnProfileChangedCompatReapply();
		return;
	}

	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING) {
		// レビュー指摘(Critical-1b、REQ-COMPAT-001、S9-7): monitoring_type はシーンコレクションに
		// 永続化されるため、切替でソースが未ロードになる前に互換モードの復元を直列に行う
		// (frontend イベントは常に同一スレッド上で同期的に配送されるため、ここでの処理と次の
		// SCENE_COLLECTION_CHANGED の処理は自然に直列になる)。
		if (g_dock)
			g_dock->OnSceneCollectionChanging();
		return;
	}

	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
		// レビュー指摘(Critical-1b): 新しいシーンコレクションの読み込み後、互換モードの設定が
		// 有効なままなら新コレクションのソースへ再適用する。
		if (g_dock)
			g_dock->OnSceneCollectionChanged();
		return;
	}

	if (event == OBS_FRONTEND_EVENT_EXIT) {
		// Dock の QTimer(状態/メーター/MONITOR_ONLY ポーリング)を先に止めてから
		// OutputInstance::Shutdown() を呼ぶ(qt-ui-and-lifecycle.md/ブリーフ実装内容: EXIT 時は
		// QTimer 停止 → control thread へ Stop 要求 → control thread join)。Dock 自体は OBS 所有の
		// ため delete しない。Shutdown() は Stop コマンドの enqueue を含むため、ここでブロックする
		// (EXIT シーケンス自体は元々同期的だったため許容する)。
		if (g_dock)
			g_dock->OnExit();
		g_output_instance.Shutdown();
		obs_log(LOG_INFO, "OBS_FRONTEND_EVENT_EXIT received; plugin unloading");
	}
}

// Tools メニュー「External Audio Output: List Devices (Debug)」のコールバック(Slice 4、
// REQ-DEVICE-001)。config.json に書く device_id をユーザーが得るためのデバッグ手段。
// obs_frontend_add_tools_menu_item のコールバックは Qt UI スレッド(STA)で呼ばれる
// (確認済み、ARCHITECTURE.md §12)。AudioDeviceManager::Enumerate() は STA からの呼び出しに
// 対応している(wasapi-com.md)。ログ形式はブリーフどおり: 1 行目に件数+既定 id、以降 1 デバイス
// 1 行。
void OnListDevicesMenuClicked(void *private_data)
{
	UNUSED_PARAMETER(private_data);

	const AudioDeviceEnumeration enumeration = AudioDeviceManager::Enumerate();
	if (!enumeration.succeeded) {
		// レビュー指摘 L-6: 失敗した HRESULT をログに残す(H-2 と同時対応)。
		obs_log(LOG_ERROR, "device list: enumeration failed (hr=0x%08lX)",
			(unsigned long)enumeration.enumeration_hr);
		return;
	}

	obs_log(LOG_INFO, "device list (%zu devices, default=%s):", enumeration.devices.size(),
		enumeration.default_device_id.empty() ? "(unknown)" : enumeration.default_device_id.c_str());
	for (size_t i = 0; i < enumeration.devices.size(); ++i) {
		const auto &device = enumeration.devices[i];
		obs_log(LOG_INFO, "  [%zu] \"%s\" id=%s", i, device.name.c_str(), device.id.c_str());
	}
}

} // namespace

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);

	// Slice 5: 本実装の Dock(出力先選択・状態表示・Level Meter・開始/停止・MONITOR_ONLY 警告・
	// 診断コピー・詳細設定)。g_output_instance への非所有ポインタを渡す(生存はプラグイン全体と
	// 同じ)。widget の所有権は OBS 側に移る(確認済み事実。delete しない)。
	auto *dock_widget = new ExternalOutputDock(&g_output_instance);
	g_dock = dock_widget;

	obs_frontend_add_dock_by_id(kDockId, obs_module_text("ExternalAudioOutput.Dock.Title"), dock_widget);

	obs_frontend_add_event_callback(OnFrontendEvent, nullptr);

	// S4: デバイス列挙結果(name + id)を OBS ログへ出す(REQ-DEVICE-001)。config.json に書く
	// device_id をユーザーが得るための手段(UI 選択は S5)。Slice 7: 「Start/Stop (Debug)」は Dock の
	// 開始/停止トグルと機能が重複するため削除し、こちらの診断用メニューのみ残す(slice-7.md
	// 実装内容 6)。
	obs_frontend_add_tools_menu_item("External Audio Output: List Devices (Debug)", OnListDevicesMenuClicked,
					 nullptr);

	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);

	// レビュー指摘(High-B/Low-G、S9-10): OBS_FRONTEND_EVENT_EXIT を経ずに終了した場合の保険。
	// 以前はここで compat-restore.json からの復元(obs_source_set_monitoring_type +
	// obs_set_audio_monitoring_device)を試みていたが、S9-10 により EXIT 相当のタイミングでの
	// monitoring_type 変更はシーンコレクションに保存されない(既に SaveProjectNow() 済み)ため
	// 無意味なうえ、終了直前に全 monitor を再生成するリスク(Low-G)があった。ここでは
	// ファイルの存在確認と WARNING ログのみに留め、実際の復元は次回起動時の
	// RestoreStaleFileIfPresent()(ApplyStartupCompatState() 経由)に委ねる。g_dock(Qt QWidget)は
	// OBS 側の破棄タイミングが obs_module_unload() に対して未確認(ARCHITECTURE.md 旧#13)なため
	// 触れず、単独の MonitorCompatController インスタンスを使う(WarnIfStaleRestoreFilePresent() は
	// output_instance_ を参照しないため安全)。
	MonitorCompatController fallback_compat_controller(&g_output_instance);
	fallback_compat_controller.WarnIfStaleRestoreFilePresent();

	// 冪等な保険: OBS_FRONTEND_EVENT_EXIT で既に Shutdown 済みなら no-op(OutputInstance::Shutdown 内で
	// control_thread_.joinable() を見て判定する)。
	g_output_instance.Shutdown();
	obs_log(LOG_INFO, "plugin unloaded");
}
