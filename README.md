# HandsOff

> **Hands off my files!** — See what's locking your files or folders and free them right inside Windows Explorer's own conflict dialog.

[![Windows 10/11 x64](https://img.shields.io/badge/Windows-10%20%2F%2011%20x64-0078D4?style=flat-square)](https://github.com/1Stalk/HandsOff/releases/latest)
[![CI](https://github.com/1Stalk/HandsOff/actions/workflows/ci.yml/badge.svg)](https://github.com/1Stalk/HandsOff/actions/workflows/ci.yml)
[![Size](https://img.shields.io/badge/Size-~201%20KB-2EA043?style=flat-square)](https://github.com/1Stalk/HandsOff/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue?style=flat-square)](LICENSE)

Tired of *"The action can't be completed because the file is open in another program"* — especially for folders, where Windows won't even tell you **which** app is holding them?

**HandsOff** upgrades Explorer's native **"File / Folder In Use"** dialog in place:

- **Names the culprit** — Shows the exact app, executable, PID, and nested file holding a lock (even on folders).
- **One-click unlock** — Adds an inline **Kill** / **Unlock** button right next to *Try Again*.
- **Auto-retries** — Frees the lock and finishes your move, rename, or delete automatically.

<p align="center">
  <img src="https://github.com/user-attachments/assets/ea2a207b-24cf-472a-843a-a6dd02c24101" alt="HandsOff demo" />
</p>

## Installation

### 1. PowerShell (Recommended)

Downloads the latest release, verifies SHA-256, installs into `%LocalAppData%\HandsOff`, and activates immediately in Explorer — no manual download needed.

**Install / Update:**

```powershell
irm https://raw.githubusercontent.com/1Stalk/HandsOff/main/install.ps1 | iex
```

**Uninstall:**

```powershell
irm https://raw.githubusercontent.com/1Stalk/HandsOff/main/uninstall.ps1 | iex
```

### 2. Via `HandsOff.exe` (Manual)

1. Download `HandsOff-*-win-x64.zip` from [Releases](https://github.com/1Stalk/HandsOff/releases/latest) and extract `HandsOff.exe` + `HandsOff.dll` into any folder.
2. Run `HandsOff.exe` and click **Install** (works immediately, no Explorer restart needed). Use **Uninstall** or **Reapply** anytime.

## Lightweight by Design

- **~201 KB total** — Pure native Win32 C++ (`HandsOff.exe` ~63 KB + `HandsOff.dll` ~138 KB) linking against the inbox Windows UCRT (`ucrtbase.dll`) with zero external runtimes or VC++ Redistributable dependencies.
- **Zero background bloat** — No tray icon, no startup daemon, no idle CPU/RAM drain. `HandsOff.dll` loads inside `explorer.exe` and only scans when a file/folder lock actually happens.
- **On-demand elevation** — If an elevated process holds the lock, `HandsOffSvc` starts for that single unlock and shuts right back down.
- **Works on folders, too** — Shows the locking app, executable, PID, and the exact nested file keeping a directory busy.
- **Clean toggle** — Hot-injects and hot-unloads from `explorer.exe` without restarting your shell.

## CLI

For scripting or inspecting locks directly from a terminal:

```powershell
.\HandsOff.exe install
.\HandsOff.exe uninstall
.\HandsOff.exe scan "D:\Path\To\Target"
.\HandsOff.exe unlock "D:\Path\To\Target"   # close handles without killing the app
.\HandsOff.exe kill "D:\Path\To\Target"     # terminate locking apps & close handles
```

## Build

Requires Visual Studio 2022 Build Tools (MSVC x64) and CMake 3.20+.

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Werror -Test
```

Outputs `HandsOff.exe` and `HandsOff.dll` to the project root and runs the CTest suite.

## How It Works

1. **Hook (`file_in_use_hook.cpp`)** — Intercepts `windows.storage.dll!GetInfoForFileInUse` and returns a custom `IFileIsInUse` implementation populated with lock details.
2. **Scanner (`lock_finder.cpp`)** — Resolves lockers across running executables, the Windows Restart Manager API, and NT kernel handles (`NtQuerySystemInformation`) with a timeout-safe path resolver.
3. **Dialog (`dialog_enhancer.cpp`)** — Subclasses `OperationStatusWindow`, embeds the inline action button, and clicks *Try Again* automatically after unlocking.

## Structure

```text
├── CMakeLists.txt                CMake & CTest build configuration
├── build.ps1                     Release x64 build & CTest helper
├── install.ps1                   One-line user installer (GitHub Releases + SHA-256)
├── uninstall.ps1                 One-line uninstaller & Explorer cleanup script
├── src/
│   ├── common.h                  Shared constants, IPC structs, and path helpers
│   ├── lock_finder.h / .cpp      3-stage lock scanner, handle closer, and process terminator
│   ├── file_in_use_hook.h / .cpp GetInfoForFileInUse hook and IFileIsInUse implementation
│   ├── dialog_enhancer.h / .cpp  OperationStatusWindow subclass and inline action button
│   ├── dllmain.cpp               COM shell extension exports, hot-unload lifecycle & CLI/service entrypoints
│   ├── hands_off.def             DLL export definitions
│   ├── gui.h / .cpp              Minimal Win32 status & patch manager window
│   ├── main.cpp                  CLI, Explorer hot-inject/unload, and --service pipe server
│   └── min_runtime.cpp           Exception-free new/delete & STL error stubs for compact binaries
└── tests/
    ├── handsoff_tests.cpp        CTest unit, lock-finder, and hook prologue compatibility suite
    └── e2e/
        ├── run_e2e.ps1           Interactive Explorer UI Automation E2E suite (7 cases)
        └── run_sandbox_e2e.ps1   Windows Sandbox wrapper for isolated E2E runs
```

## Contributing

Pull requests and issues are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md) for build & test instructions.

## Tests

Unit, lock-finder & hook tests:

```powershell
ctest --test-dir build --output-on-failure --no-tests=error
```

Live Explorer E2E (run as Administrator to include `HandsOffSvc`):

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\e2e\run_e2e.ps1
```

Isolated Windows Sandbox E2E:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\e2e\run_sandbox_e2e.ps1
```

## Tested Windows Systems

| OS | Version | Build | Locale | Environment | Result |
| :--- | :--- | :--- | :--- | :--- | :--- |
| Windows 11 Pro for Workstations / Enterprise | 24H2 | `26100.9457` (x64) | `en-US` | Host + Windows Sandbox | **Pass (7/7 E2E)** |
| Windows 11 Pro | 23H2 | `22631.3880` (x64) | `ru-RU` | Hyper-V VM | **Pass (7/7 E2E)** |
| Windows 10 Pro | 22H2 | `19045.5487` (x64) | `ru-RU` | Hyper-V VM | **Pass (7/7 E2E)** |
| Windows Server 2022 & 2025 | CI Runners | `Server 2022 / 2025` (x64) | `en-US` | GitHub Actions CI | **Pass (Build + CTest + CLI)** |

> **Tested on another Windows build or locale?** Contributions don't have to be code — adding your system here helps everyone!
> Run `run_e2e.ps1` above, check `Win+R` → `winver` (OS, Version, Build), and open a PR with your row.

## License

[MIT](LICENSE)
