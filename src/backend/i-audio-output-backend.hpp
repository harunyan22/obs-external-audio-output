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

// 対応 REQ: REQ-AUDIO-002, REQ-DEVICE-001 / Slice 2, Slice 4
//
// 音声出力バックエンドの最小抽象 IF。S2 は Windows/WASAPI(WasapiOutput)のみが実装するが、
// 将来の macOS/CoreAudio バックエンド追加(buffer_chunks の "backend" 設定キー参照)に備えて
// OutputInstance からはこの IF 越しにのみ操作する。

#pragma once

#include <cstdint>
#include <string>

class RingBuffer;

class IAudioOutputBackend {
public:
	virtual ~IAudioOutputBackend() = default;

	// デバイスを開き、フォーマット交渉・ネイティブリソースの初期化を行う(呼び出しは control
	// スレッドから。ブロックして完了を待つ)。device_id は解決済みの出力先("default" または
	// バックエンド固有のデバイス識別子文字列。WASAPI では IMMDevice::GetId() の値、Slice 4)。
	// id の突き合わせ(保存値 vs 列挙結果)は呼び出し元(OutputInstance + device-matcher)の責務で、
	// ここでは解決済みの id をそのまま使う。成功時 out_sample_rate にデバイスのミックスレートを
	// 書く。RingBuffer はまだ受け取らない(そのサイズはデバイスレート判明後に決まるため。
	// レビュー指摘 #2、BindSource 参照)。
	virtual bool Open(const std::string &device_id, uint32_t *out_sample_rate) = 0;

	// Open() 成功後・Start() 前に 1 回呼ぶ(control スレッド、RT ではない)。source は以後の
	// Start()/Stop() を通じて読み取り専用で保持される(所有権は移らない)。source->MaxChunkFrames()
	// に合わせて内部バッファを確保する実装を想定(構築時 1 回の確保、レビュー指摘 #2)。
	virtual void BindSource(RingBuffer *source) = 0;

	// 実際のレンダリングを開始する。Open()/BindSource() 成功後のみ呼べる。呼び出しはブロックして
	// 開始成否を返す(RT ループでの失敗は Stop() を待たず false 以外の経路で検出される)。
	virtual bool Start() = 0;

	// 停止・スレッド join・ネイティブリソース解放。Open()/Start() 前の状態でも安全(冪等)。
	virtual void Stop() = 0;

	// レンダリング中に回復不能なエラーが起きた場合に true を返す(control スレッドがポーリングで検出)。
	virtual bool HasFailed() const = 0;
};
