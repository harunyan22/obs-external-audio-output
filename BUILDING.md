# Building from source

## Requirements
- Windows 10/11 x64
- Visual Studio 2022 (Build Tools 17.x) with C++ workload
- CMake 3.28+ (4.0.1 tested)
- Windows SDK 10.0.22621 (selected by the Windows preset)
- Git

## Build
```powershell
cmake --preset windows-x64 -DPLUGIN_BUILD_NUMBER=1
cmake --build --preset windows-x64 --config RelWithDebInfo
```
Dependencies (libobs headers, obs-deps, Qt6 prebuilt) are downloaded automatically into `.deps/` on first configure (per `buildspec.json`).

Artifacts:
- `build_x64\RelWithDebInfo\obs-external-audio-output.dll`
- Locale files: `data\locale\*.ini`

## Install layout
```
C:\ProgramData\obs-studio\plugins\obs-external-audio-output\
  bin\64bit\obs-external-audio-output.dll
  data\locale\en-US.ini, ja-JP.ini
```

## Tests
```powershell
ctest --test-dir build_x64 -C RelWithDebInfo
```
