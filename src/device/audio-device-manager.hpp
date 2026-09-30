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
// WASAPI 出力エンドポイントの列挙(wasapi-com.md 準拠)。使い捨てのヘルパで、Enumerate() 呼び出し
// 1 回ごとに呼び出しスレッドで CoInitializeEx(MTA)/CoUninitialize を対で行う。RPC_E_CHANGED_MODE
// (呼び出しスレッドが既に別モデルで初期化済み)は成功扱いとし、その場合 CoUninitialize は呼ばない。
//
// 呼び出し元スレッドを問わず安全に呼べる設計にしてある: Qt UI スレッド(STA、Tools メニュー経由)
// からも、WASAPI render thread(MTA、セットアップフェーズ)からも呼び得る
// (MMDeviceEnumerator は Both モデルのため STA からの列挙も可、ARCHITECTURE.md §12)。

#pragma once

#include "device-matcher.hpp"

#include <string>
#include <vector>

// 列挙結果。succeeded == false はデバイス列挙 API 自体が失敗したことを示す(devices が単に空
// なのとは区別する)。default_device_id は取得できなければ空文字列のまま。
struct AudioDeviceEnumeration {
	bool succeeded = false;
	std::vector<device_matcher::DeviceEntry> devices;
	std::string default_device_id;
	// succeeded == false のときに失敗した HRESULT(レビュー指摘 H-2/L-6)。呼び出し元がログに
	// 出せるよう HRESULT の実体(long)のまま保持する。このヘッダを COM ヘッダに依存させないため
	// <windows.h> の HRESULT 型ではなく long で持つ(HRESULT は typedef long HRESULT のため
	// 実体は同じ)。succeeded == true のときは 0 のまま。
	long enumeration_hr = 0;
};

class AudioDeviceManager {
public:
	// IMMDeviceEnumerator::EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE) でアクティブな出力
	// エンドポイントを列挙し、GetDefaultAudioEndpoint(eRender, eConsole) の id も同じ COM
	// セッション内で取得する。失敗時は succeeded=false・devices は空のまま返す(呼び出し元がログを
	// 出すこと)。
	static AudioDeviceEnumeration Enumerate();
};
