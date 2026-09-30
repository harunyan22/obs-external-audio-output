cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/audio/audio-mix-source.cpp
    src/audio/audio-mix-source.hpp
    src/audio/track-map.hpp
    src/audio/ring-buffer.hpp
    src/audio/drift-compensator.hpp
    src/backend/i-audio-output-backend.hpp
    src/backend/windows/wasapi-output.cpp
    src/backend/windows/wasapi-output.hpp
    src/compat/compat-plan.hpp
    src/compat/monitor-compat-controller.cpp
    src/compat/monitor-compat-controller.hpp
    src/config/plugin-config.cpp
    src/config/plugin-config.hpp
    src/device/device-matcher.hpp
    src/device/audio-device-manager.cpp
    src/device/audio-device-manager.hpp
    src/device/device-notifier.hpp
    src/output/output-instance.cpp
    src/output/output-instance.hpp
    src/output/control-queue.hpp
    src/output/reconnect-policy.hpp
    src/diag/diagnostics.cpp
    src/diag/diagnostics.hpp
    src/diag/diagnostics-text.hpp
    src/ui/external-output-dock.cpp
    src/ui/external-output-dock.hpp
)

if(WIN32)
  # WasapiOutput(src/backend/windows): COM(CoCreateInstance 等)は ole32、MMCSS
  # (AvSetMmThreadCharacteristics)は avrt。
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ole32 avrt)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# tests/unit: libobs にリンクしない単体テスト。プラグイン本体のビルドには影響しない。
# include(CTest) が BUILD_TESTING オプション(既定 ON)を定義し enable_testing() も呼ぶ(CTest モジュールの規約)。
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests/unit)
endif()
