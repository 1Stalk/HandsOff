# HandsOff

**Hands off from my files!** 

Windows loves saying *"The action can't be completed because the file is open in another program"* — especially for folders, where it doesn't even tell you which program. **HandsOff** fixes Explorer's own conflict dialog so it names the exact app holding your file or folder and adds a **Kill** / **Unlock** button right next to *Try Again* — one click, and your move, rename, or delete finishes on its own.

## Installation

### 1. PowerShell (Recommended)

Downloads the latest release, verifies SHA-256, installs into `%LocalAppData%\HandsOff`, and activates immediately in Explorer — no manual download needed:

```powershell
# Install (or update)
irm https://raw.githubusercontent.com/1Stalk/HandsOff/main/install.ps1 | iex

# Uninstall & remove completely
irm https://raw.githubusercontent.com/1Stalk/HandsOff/main/uninstall.ps1 | iex
```

### 2. Via `HandsOff.exe` (Manual)

1. Download `HandsOff-*-win-x64.zip` from [Releases](https://github.com/1Stalk/HandsOff/releases/latest) and extract `HandsOff.exe` + `HandsOff.dll` into any folder.
2. Run `HandsOff.exe` and click **Install** (works immediately, no Explorer restart needed). Use **Uninstall** or **Reapply** anytime.

## Lightweight by Design

- **~225 KB total** — Pure native Win32 C++ (`HandsOff.exe` ~103 KB + `HandsOff.dll` ~122 KB) with zero external runtimes or dependencies.
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
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

Outputs `HandsOff.exe` and `HandsOff.dll` to the project root.

## How It Works

1. **Hook (`file_in_use_hook.cpp`)** — Intercepts `windows.storage.dll!GetInfoForFileInUse` and returns a custom `IFileIsInUse` implementation populated with lock details.
2. **Scanner (`lock_finder.cpp`)** — Resolves lockers across running executables, the Windows Restart Manager API, and NT kernel handles (`NtQuerySystemInformation`) with a timeout-safe path resolver.
3. **Dialog (`dialog_enhancer.cpp`)** — Subclasses `OperationStatusWindow`, embeds the inline action button, and clicks *Try Again* automatically after unlocking.

## Structure

```text
src/
├── common.h                 Shared constants, IPC structs, and path helpers
├── lock_finder.h / .cpp     3-stage lock scanner, handle closer, and process terminator
├── file_in_use_hook.h/.cpp  GetInfoForFileInUse hook and IFileIsInUse implementation
├── dialog_enhancer.h / .cpp OperationStatusWindow subclass and inline action button
├── dllmain.cpp              COM shell extension exports and hot-unload lifecycle
├── hands_off.def            DLL export definitions
├── gui.h / .cpp             Minimal Win32 status & patch manager window
└── main.cpp                 CLI, Explorer hot-inject/unload, and --service pipe server
```
