# 対応 REQ: REQ-AUDIO-001 / Slice 1
#
# libobs にリンクしない単体テスト。track-map.hpp は OBS ヘッダに依存しないため、
# doctest(vendoring 済み単一ヘッダ)とだけリンクしてビルドする。
# プラグイン本体(${CMAKE_PROJECT_NAME})のビルド・リンクには一切影響しない。

add_executable(track-map-tests test-track-map.cpp)

target_include_directories(track-map-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME track-map-tests COMMAND track-map-tests)

# 対応 REQ: REQ-AUDIO-002, REQ-NFR-002 / Slice 2
#
# ring-buffer.hpp も OBS ヘッダに依存しないため、doctest とだけリンクしてビルドする。

add_executable(ring-buffer-tests test-ring-buffer.cpp)

target_include_directories(ring-buffer-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME ring-buffer-tests COMMAND ring-buffer-tests)

# 対応 REQ: REQ-AUDIO-005, REQ-NFR-002 / Slice 3
#
# drift-compensator.hpp も OBS/Windows ヘッダに依存しないため、doctest とだけリンクしてビルドする。

add_executable(drift-compensator-tests test-drift-compensator.cpp)

target_include_directories(drift-compensator-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME drift-compensator-tests COMMAND drift-compensator-tests)

# 対応 REQ: REQ-DEVICE-001 / Slice 4
#
# device-matcher.hpp も OBS/COM ヘッダに依存しないため、doctest とだけリンクしてビルドする。

add_executable(device-matcher-tests test-device-matcher.cpp)

target_include_directories(device-matcher-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME device-matcher-tests COMMAND device-matcher-tests)

# 対応 REQ: REQ-LOG-001, REQ-UI-008 / Slice 5
#
# diagnostics-text.hpp(AC-22 の 15 項目の整形ロジック)も libobs/Windows API に依存しないため、
# doctest とだけリンクしてビルドする。

add_executable(diagnostics-text-tests test-diagnostics-text.cpp)

target_include_directories(diagnostics-text-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME diagnostics-text-tests COMMAND diagnostics-text-tests)

# 対応 REQ: REQ-DEVICE-002, REQ-DEVICE-003 / Slice 6
#
# reconnect-policy.hpp も libobs/Windows API に依存しないため、doctest とだけリンクしてビルドする。

add_executable(reconnect-policy-tests test-reconnect-policy.cpp)

target_include_directories(reconnect-policy-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME reconnect-policy-tests COMMAND reconnect-policy-tests)

# 対応 REQ: REQ-DEVICE-002, REQ-DEVICE-003, REQ-DEVICE-004, REQ-STABILITY-001 / Slice 6
#
# control-queue.hpp も libobs/Windows API に依存しないため、doctest とだけリンクしてビルドする
# (std::thread を使うためスレッドライブラリのリンクが必要な環境では CMake の Threads パッケージ経由に
# なるが、MSVC/Windows では追加のリンク設定なしで std::thread が使える)。

add_executable(control-queue-tests test-control-queue.cpp)

target_include_directories(control-queue-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME control-queue-tests COMMAND control-queue-tests)

# 対応 REQ: REQ-CONFIG-001, REQ-CONFIG-002 / Slice 7
#
# plugin-config.hpp の値域クランプ・schema_version 判定・既定値も libobs に依存しないため、
# doctest とだけリンクしてビルドする(Load()/Save() は plugin-config.cpp 側にあり対象外)。

add_executable(plugin-config-tests test-plugin-config.cpp)

target_include_directories(plugin-config-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME plugin-config-tests COMMAND plugin-config-tests)

# 対応 REQ: REQ-COMPAT-001 / Slice 9
#
# compat-plan.hpp(互換モードの対象ソース選定・復元プラン・自己無音判定)も libobs に依存しないため、
# doctest とだけリンクしてビルドする(monitor-compat-controller.cpp の libobs 依存部分は対象外)。

add_executable(compat-plan-tests test-compat-plan.cpp)

target_include_directories(compat-plan-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME compat-plan-tests COMMAND compat-plan-tests)
