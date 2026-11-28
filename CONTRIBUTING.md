# Contributing to HandsOff

Pull requests, compatibility reports, and bug reports are welcome! Because `HandsOff.dll` runs inside `explorer.exe` and `HandsOffSvc` can run elevated on demand, please keep changes focused, small, and tested locally on Windows.

## Core Principles

- **Zero external dependencies** — pure native Win32 C++17 and Windows SDK only (`~201 KB` total, no VC++ Redistributable required).
- **Zero background footprint** — no tray icons, startup daemons, or idle polling loops.
- **No PDB symbol downloads** — hook only stable public exports and documented Win32/COM APIs.
- **Clean diffs** — never commit `.exe`, `.dll`, `.pdb`, or `build/` outputs.

## Build & Test

Requires Windows 10/11 (x64), Visual Studio 2022 Build Tools (MSVC x64), and CMake 3.20+:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Werror -Test
```

If your PR modifies the Explorer hook, dialog UI, or elevation service, also run the Explorer UI E2E suite (either directly in an elevated PowerShell session or isolated in Windows Sandbox):

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\e2e\run_e2e.ps1
# or isolated in Windows Sandbox:
powershell -ExecutionPolicy Bypass -File .\tests\e2e\run_sandbox_e2e.ps1
```

## Compatibility Reports (No-Code PRs Welcome)

You don't need to touch C++ to contribute. After running `run_e2e.ps1` (as Administrator for full coverage), check `Win+R` → `winver` for your Windows edition, **Version**, and **OS Build** (also printed in the test summary and saved to `artifacts\e2e\results.json`), then open a PR adding a row to **Tested Windows Systems** in [README.md](README.md) — or open an issue with the output if any check fails.
