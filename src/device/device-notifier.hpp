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

// 対応 REQ: REQ-DEVICE-003, REQ-DEVICE-004 / Slice 6
//
// IMMNotificationClient の自前実装(wasapi-com.md、ARCHITECTURE.md §12 旧#22 準拠)。
// 確認済み事実(旧#22、MS Learn 直読 + OBS 31.1.1 実装 wasapi-notify.cpp:50-51,65,74):
//   - コールバックのスレッドは公式に非公開。禁止 4 項目: (1) ノンブロッキング必須 (2) コールバック中に
//     同期オブジェクトを待たない (3) コールバック内から Register/Unregister を呼ばない
//     (4) コールバック中に MMDevice API オブジェクトの最終参照を解放しない。
//   - RegisterEndpointNotificationCallback/Unregister は AddRef/Release を呼ばない。参照カウント管理は
//     呼び出し側(このクラス)の責任: 生成時に ref_=1 とし、Unregister 後に Release() で解放すること。
// コールバックは ControlQueue<Event> への Push() のみを行う(enqueue のみ、ロックは
// control-queue.hpp の短時間 mutex のみ。禁止 4 項目に「ロック取得禁止」は含まれないため許容。
// control-queue.hpp 冒頭コメント参照)。Register()/Unregister() は control thread からのみ呼ぶ
// (COM 呼び出しを伴うため RT ではない)。

#pragma once

#include "../output/control-queue.hpp"

#include <windows.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <string>
#include <utility>

namespace device_notifier {

enum class EventType {
	// role==eConsole の既定出力デバイス変更(wasapi-com.md: eConsole 以外は無視。control thread 側で
	// 200ms デバウンスする)。
	DefaultDeviceChanged,
	// デバイスが新規追加された、またはアクティブ状態になった(ブリーフ実装内容 8: Recovering 中は
	// バックオフをリセットして即再接続を試みる)。
	DeviceArrived,
};

struct Event {
	EventType type = EventType::DefaultDeviceChanged;
	// DefaultDeviceChanged: 新しい既定デバイスの id(UTF-8)。DeviceArrived: 対象デバイスの id
	// (呼び出し元は現在 device_id=="default" かどうかだけを見て使わないことが多いが、診断ログ用に
	// 保持する)。
	std::string device_id;
};

} // namespace device_notifier

class DeviceNotifier final : public IMMNotificationClient {
public:
	// events: control thread が WaitPop()/TryPop() で読み出すキュー(非所有。DeviceNotifier より
	// 長生きすること。呼び出し元である OutputInstance の control thread が所有し、Unregister() +
	// Release() の後まで生存させる)。
	explicit DeviceNotifier(ControlQueue<device_notifier::Event> *events) : events_(events) {}

	DeviceNotifier(const DeviceNotifier &) = delete;
	DeviceNotifier &operator=(const DeviceNotifier &) = delete;

	// 生成直後(ref_==1)に呼ぶ。成功したら registered_=true。COM 呼び出しを伴うため control thread
	// から同期的に呼ぶ(旧#22: Register 自体は AddRef しない)。
	bool Register(IMMDeviceEnumerator *enumerator)
	{
		if (!enumerator || registered_)
			return false;
		const HRESULT hr = enumerator->RegisterEndpointNotificationCallback(this);
		registered_ = SUCCEEDED(hr);
		return registered_;
	}

	// Register() と対。呼んだ後、呼び出し元が Release() を呼んで ref_ を 1→0 にすること(旧#22:
	// Unregister は Release しない)。
	void Unregister(IMMDeviceEnumerator *enumerator)
	{
		if (!enumerator || !registered_)
			return;
		enumerator->UnregisterEndpointNotificationCallback(this);
		registered_ = false;
	}

	// レビュー指摘(High-2a): Unregister() の「前」に呼ぶ。以後、in-flight で既に呼び出し中だった
	// コールバック、および Unregister() と競合するタイミングで発火したコールバックは、すべて
	// events_ に触れず即座に return する(disabled_ のチェックが各コールバックの先頭にある)。
	// OutputInstance 側は Unregister() 後もこのインスタンスを delete しない(意図的リーク、
	// output-instance.hpp の device_notifier_ コメント参照)ため、disabled_==true 以降は「delete
	// されない安全なオブジェクトへの、何もしないコールバック呼び出し」だけが起こり得る。
	void Disable() { disabled_.store(true, std::memory_order_release); }

	// --- IUnknown ---
	// 旧#22: Register/Unregister は AddRef/Release を呼ばない前提のため、この参照カウントは
	// 「呼び出し側が生成時に 1、Unregister 後に Release() で 0」という単純なライフサイクルのみを
	// 想定する(QueryInterface 経由で複数箇所から保持されることは無い設計)。
	ULONG STDMETHODCALLTYPE AddRef() override { return ref_.fetch_add(1, std::memory_order_relaxed) + 1; }

	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG new_ref = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
		if (new_ref == 0)
			delete this;
		return new_ref;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv_object) override
	{
		if (!ppv_object)
			return E_POINTER;
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
			*ppv_object = static_cast<IMMNotificationClient *>(this);
			AddRef();
			return S_OK;
		}
		*ppv_object = nullptr;
		return E_NOINTERFACE;
	}

	// --- IMMNotificationClient ---
	// すべて禁止 4 項目準拠: ノンブロッキング、同期待ちなし、Register/Unregister の再入なし、
	// MMDevice オブジェクトの最終参照解放なし(このクラスは IMMDevice を一切保持しない)。
	// enqueue のみで即座に返る。

	HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role,
							 LPCWSTR pwstr_default_device_id) override
	{
		// レビュー指摘(High-2a): Disable() 済みなら events_ に一切触れず即座に return する。
		if (disabled_.load(std::memory_order_acquire))
			return S_OK;
		// wasapi-com.md: 出力(eRender)以外、role が eConsole 以外は無視する。
		if (flow != eRender || role != eConsole)
			return S_OK;
		device_notifier::Event event;
		event.type = device_notifier::EventType::DefaultDeviceChanged;
		event.device_id = WideToUtf8(pwstr_default_device_id);
		if (events_)
			events_->Push(std::move(event));
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstr_device_id) override
	{
		// レビュー指摘(High-2a)。
		if (disabled_.load(std::memory_order_acquire))
			return S_OK;
		device_notifier::Event event;
		event.type = device_notifier::EventType::DeviceArrived;
		event.device_id = WideToUtf8(pwstr_device_id);
		if (events_)
			events_->Push(std::move(event));
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR /*pwstr_device_id*/) override
	{
		// REQ-DEVICE-002(切断)は WASAPI render thread 側の AUDCLNT_E_DEVICE_INVALIDATED 検出
		// (HasFailed())が主経路(wasapi-com.md)。OnDeviceRemoved は Recovering 中の即時再試行トリガと
		// しては使わない(切断直後にまだデバイスが ACTIVE 一覧に残っていることがあるため、
		// OnDeviceStateChanged(ACTIVE)/OnDeviceAdded の到着イベントのみを再接続トリガに使う設計)。
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstr_device_id, DWORD new_state) override
	{
		// レビュー指摘(High-2a)。
		if (disabled_.load(std::memory_order_acquire))
			return S_OK;
		if (new_state != DEVICE_STATE_ACTIVE)
			return S_OK;
		device_notifier::Event event;
		event.type = device_notifier::EventType::DeviceArrived;
		event.device_id = WideToUtf8(pwstr_device_id);
		if (events_)
			events_->Push(std::move(event));
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR /*pwstr_device_id*/,
							 const PROPERTYKEY /*key*/) override
	{
		return S_OK; // 関知しない(表示名変更等は次回列挙で自然に反映される)。
	}

private:
	// UTF-8 変換はヒープ確保を伴うが、rt-audio.md の対象(libobs audio callback / WASAPI render
	// thread)ではなく COM 通知スレッド(公式非公開、禁止 4 項目に確保禁止は含まれない)なので許容する。
	static std::string WideToUtf8(LPCWSTR wide)
	{
		if (!wide || !*wide)
			return std::string();
		const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
		if (len <= 1)
			return std::string();
		std::string result(static_cast<size_t>(len - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), len, nullptr, nullptr);
		return result;
	}

	std::atomic<ULONG> ref_{1};
	ControlQueue<device_notifier::Event> *events_ = nullptr;
	bool registered_ = false; // control thread のみが読み書き(Register/Unregister はそこでのみ呼ぶ)。
	// レビュー指摘(High-2a): Disable() が立て、以後すべてのコールバックが先頭で参照する。
	// コールバックのスレッドは公式に非公開(クラス冒頭コメント)なので atomic にする。
	std::atomic<bool> disabled_{false};
};
