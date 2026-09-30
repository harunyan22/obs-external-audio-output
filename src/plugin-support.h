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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

extern const char *PLUGIN_NAME;
extern const char *PLUGIN_VERSION;

void obs_log(int log_level, const char *format, ...);
extern void blogva(int log_level, const char *format, va_list args);

// Slice 5(REQUIREMENTS.md §3 log_level、D-18): obs_log() が blog() へ転送する際の閾値。
// LOG_ERROR(100) < LOG_WARNING(200) < LOG_INFO(300) < LOG_DEBUG(400、util/base.h)。
// level > threshold のメッセージは抑制する(数値が大きいほど詳細)。OutputInstance::Restart() が
// config.json の log_level(または Advanced 設定変更)を反映するたびに呼ぶ。UI/frontend イベント
// スレッドから呼ぶ想定で、RT スレッド(callback/render thread)からは呼ばない。
void obs_log_set_level_threshold(int log_level);

// Slice 5 レビュー指摘 M-7: g_log_level_threshold による抑制の対象外にしたいログ専用の経路
// (AC-22 の証跡: 開始時ダンプ・診断スナップショット・停止サマリ)。obs_log() と違い閾値を一切見ずに
// 常に blog() へ転送する。乱用しない(この 3 種類の診断ログ以外からは呼ばない)。
void obs_log_always(int log_level, const char *format, ...);

#ifdef __cplusplus
}
#endif
