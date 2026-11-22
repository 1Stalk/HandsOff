# Contributing to HandsOff

Pull requests and bug reports are welcome! Because `HandsOff.dll` runs inside `explorer.exe` and `HandsOffSvc` can run elevated on demand, please keep changes focused, small, and tested locally on Windows.

## Core Principles

- **Zero external dependencies** — pure native Win32 C++17 and Windows SDK only (target ~225 KB total).
- **Zero background footprint** — no tray icons, startup daemons, or idle polling loops.
- **No PDB symbol downloads** — hook only stable public exports and documented Win32/COM APIs.
- **Clean diffs** — never commit `.exe`, `.dll`, `.pdb`, or `build/` outputs.

## Build & Test

Requires Windows 10/11 (x64), Visual Studio 2022 Build Tools (MSVC x64), and CMake 3.20+:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Werror -Test
```

If your PR modifies the Explorer hook, dialog UI, or elevation service, also verify `.\HandsOff.exe install` and `.\HandsOff.exe uninstall` against a real locked file/folder in File Explorer.
