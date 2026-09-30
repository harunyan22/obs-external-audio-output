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

#include "audio-device-manager.hpp"

#include <wrl/client.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <propsys.h>
// PKEY_Device_FriendlyName に実体(ストレージ)を持たせるため、functiondiscoverykeys_devpkey.h を
// 取り込む直前に <initguid.h> を include する(レビュー指摘 L-10: #define INITGUID 方式から変更。
// initguid.h は「次に include される DEFINE_PROPERTYKEY 系ヘッダに実体を持たせる」効果を持つため、
// functiondiscoverykeys_devpkey.h の直前という位置が重要。このファイル限定の効果で、
// wasapi-output.cpp 側には影響しない)。
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

using Microsoft::WRL::ComPtr;

namespace {

// COM 初期化ガード(レビュー指摘 H-1)。コンストラクタで CoInitializeEx(MTA)、デストラクタで
// 自分が初期化した場合のみ CoUninitialize する。Enumerate() の先頭でこれを最初に構築し、
// それ以降に宣言する全 ComPtr(enumerator 含む)がこれより後に破棄されるスコープ構造にすることで、
// 「COM インターフェースの Release が CoUninitialize より後になる」事故を型で防ぐ(旧実装は
// enumerator/default_device の破棄が関数末尾でこの CoUninitialize より後に起きていた)。
// ヘッダのコメントが謳う「呼び出しスレッドを問わず安全」を実装として保証する。
class ComScope {
public:
	ComScope()
	{
		hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		owned_ = SUCCEEDED(hr_);
		usable_ = SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
	}
	~ComScope()
	{
		if (owned_)
			CoUninitialize();
	}

	ComScope(const ComScope &) = delete;
	ComScope &operator=(const ComScope &) = delete;

	bool usable() const { return usable_; }
	HRESULT hr() const { return hr_; }

private:
	HRESULT hr_ = E_FAIL;
	bool owned_ = false;
	bool usable_ = false;
};

// wide 文字列(WASAPI の GetId()/PKEY_Device_FriendlyName が返す形)を UTF-8 の std::string へ
// 変換する。エンドポイント ID は実質 ASCII だが、friendly name は非 ASCII を含み得るため
// CP_UTF8 経由で変換する。
std::string WideToUtf8(const wchar_t *wide)
{
	if (!wide || !*wide)
		return std::string();
	const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
	if (len <= 1)
		return std::string();
	std::string result(static_cast<size_t>(len - 1), '\0'); // len には終端 NUL 分が含まれる
	WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), len, nullptr, nullptr);
	return result;
}

// IMMDevice から {id, friendly name} を取り出す。取得に失敗した項目は空文字列のまま返す
// (呼び出し元は id が空なら丸ごとスキップする)。
device_matcher::DeviceEntry ReadDeviceEntry(IMMDevice *device)
{
	device_matcher::DeviceEntry entry;

	LPWSTR id_wide = nullptr;
	if (SUCCEEDED(device->GetId(&id_wide)) && id_wide) {
		entry.id = WideToUtf8(id_wide);
		CoTaskMemFree(id_wide);
	}

	ComPtr<IPropertyStore> props;
	if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.GetAddressOf()))) {
		PROPVARIANT name_var;
		PropVariantInit(&name_var);
		if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name_var)) && name_var.vt == VT_LPWSTR &&
		    name_var.pwszVal) {
			entry.name = WideToUtf8(name_var.pwszVal);
		}
		PropVariantClear(&name_var);
	}

	return entry;
}

} // namespace

AudioDeviceEnumeration AudioDeviceManager::Enumerate()
{
	AudioDeviceEnumeration result;

	// wasapi-com.md: 呼び出しスレッドで CoInitializeEx(MTA)。RPC_E_CHANGED_MODE は初期化済み扱いで
	// CoUninitialize しない(Qt UI スレッドは STA、WASAPI render thread は既に MTA 初期化済みの
	// どちらから呼ばれても安全にする)。ComScope を関数内で最初に宣言することで、以降に宣言する
	// 全 ComPtr(enumerator 含む)が関数を抜けるときに必ず ComScope より先に破棄される(レビュー指摘
	// H-1、ComScope の定義コメント参照)。
	const ComScope com_scope;
	if (!com_scope.usable()) {
		result.enumeration_hr = com_scope.hr();
		return result;
	}

	ComPtr<IMMDeviceEnumerator> enumerator;
	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
				      (void **)enumerator.GetAddressOf());
	if (FAILED(hr)) {
		result.enumeration_hr = hr;
		return result;
	}

	ComPtr<IMMDeviceCollection> collection;
	hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.GetAddressOf());
	if (SUCCEEDED(hr)) {
		UINT count = 0;
		collection->GetCount(&count);
		result.devices.reserve(count);
		for (UINT i = 0; i < count; ++i) {
			ComPtr<IMMDevice> device;
			if (FAILED(collection->Item(i, device.GetAddressOf())))
				continue;
			device_matcher::DeviceEntry entry = ReadDeviceEntry(device.Get());
			if (!entry.id.empty())
				result.devices.push_back(std::move(entry));
		}
		result.succeeded = true;
	} else {
		// レビュー指摘 H-2: 列挙失敗を hr 付きで記録する。呼び出し元(OutputInstance)が
		// 「succeeded==false」を「0 件」と誤診断しないよう、devices を空のまま succeeded=false・
		// enumeration_hr にこの HRESULT を残して返す。
		result.enumeration_hr = hr;
	}

	// EnumAudioEndpoints が失敗していても、enumerator 自体は生きているので既定デバイスの取得は
	// 試みる(ベストエフォート。取得できなければ default_device_id は空文字列のまま)。
	ComPtr<IMMDevice> default_device;
	if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, default_device.GetAddressOf()))) {
		LPWSTR id_wide = nullptr;
		if (SUCCEEDED(default_device->GetId(&id_wide)) && id_wide) {
			result.default_device_id = WideToUtf8(id_wide);
			CoTaskMemFree(id_wide);
		}
	}

	return result;
}
