# Infinifusion

Standalone JSON modding for SMTV Vengeance Steam.

Supports reflected UDataTable rows, ScriptMessageAsset entries, MMIAsset TBCR records and reflected UObject/Blueprint properties. For version 1.0.3a Steam build 17140961, haven't tested on other versions.

## Build

### Visual Studio (Recommended)

1. Open `Infinifusion.sln` in Visual Studio 2022 (with Desktop development with C++).
2. Select **Release** and **x64**.
3. Build the solution (`Ctrl+Shift+B`).

The compiled `winmm.dll` and `infinifusion.ini` will be output to `bin/Release/`.

### MSBuild (Command Line)

In a Developer Command Prompt or PowerShell:

```powershell
msbuild Infinifusion.sln /p:Configuration=Release /p:Platform=x64
```

### CMake

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Install

- Copy `winmm.dll` and `infinifusion.ini` to the runtime folder on `Project/Binaries/Win64`.

- Place each JSON mod folder (containing `mod.json`) under `Project/Content/Paks/~mods/infinifusion`.

## Tips

- F6 reloads changed definitions, some changes require a menu reopening, a fresh asset load, or restarting the
game.

- For troubleshooting, set `[Logging] DebugMode=true` in `infinifusion.ini`, restart, reproduce the issue, then close the game and send `infinifusion.log` with the mod versions and reproduction steps, `infinifusion.previous.log` retains the preceding run. Set it back to false for playing normal.

- To uninstall a manual copy, close the game and remove the installed DLL.

Copyright (c) 2026 mugenrei.