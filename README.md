# GoXLR Keep-Alive

A lightweight system tray utility that prevents audio distortion and OBS freezes associated with GoXLR and GoXLR Mini devices on Windows.

## The Issue

This tool addresses a specific bug caused by how Windows handles idle GoXLR audio channels (such as Music, Sound Alerts, or Game channels). Symptoms include:

* An audio channel suddenly begins crackling, buzzing, or producing distorted audio after a period of inactivity.
* Adjusting the volume slider for that channel in OBS has no effect.
* Shortly after the audio distortion begins, OBS locks up entirely and requires a force-restart.
* Reassigning Windows sound devices temporarily resolves the issue.

**Cause:** Windows utilizes a power-saving feature that puts inactive audio channels to sleep. When a channel wakes up, the OBS WASAPI connection to that endpoint goes stale, causing audio corruption and eventually deadlocking the OBS audio thread.

## How it Works

GoXLR Keep-Alive runs silently in the background and sends an inaudible, silent audio stream to all GoXLR endpoints. This prevents Windows from putting the channels to sleep, keeping the OBS session handles valid. The application has a minimal footprint (~150 KB) and uses virtually zero system resources.

## Installation

### Pre-built Binary (Recommended)
1. Download `GoXLRKeepAlive.exe` from the [Releases](../../releases) page.
2. Run the executable. A green indicator will appear in your system tray confirming it is active.

### Build from Source
1. Open `GoXLRKeepAlive.sln` in Visual Studio 2022 (requires the "Desktop development with C++" workload).
2. Build the **Release | x64** configuration.
3. The compiled executable will be located in `bin\Release\`.

## Run on Startup

To have the utility run automatically when Windows starts:
1. Press `Win + R`, type `shell:startup`, and press Enter.
2. Place a shortcut to `GoXLRKeepAlive.exe` inside the Startup folder.

Alternatively, you can create the shortcut using PowerShell:
```powershell
$s = (New-Object -ComObject WScript.Shell).CreateShortcut("$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Startup\GoXLRKeepAlive.lnk")
$s.TargetPath = "C:\path\to\GoXLRKeepAlive.exe"
$s.Save()
```

## System Tray Options

Clicking the tray icon provides the following options:

| Option | Description |
|--------|-------------|
| **Status** | Displays how many GoXLR channels are currently kept awake. The icon is green when healthy, and red if an error occurs. |
| **Show Details** | Lists the status of every GoXLR endpoint. |
| **Restart Sessions** | Re-scans for GoXLR devices (useful after unplugging/replugging the device). |
| **Quit** | Terminates the application. |

## Notes & FAQ

* **Audio Impact:** The utility sends digital silence (all zeros). It does not alter your actual audio mix or add noise to your stream.
* **Compatibility:** Works with both the official TC-Helicon GoXLR app and the open-source GoXLR Utility.
* **Device Disconnects:** If the GoXLR is unplugged, the app attempts to reconnect automatically within 5 seconds. If it fails, use the "Restart Sessions" option in the tray menu.
* **Other Recommended Fixes:** For optimal stability, it is still recommended to disable **USB Selective Suspend** (via Windows Power Options) and ensure all GoXLR endpoints are set to the same sample rate (48000 Hz).
* **SmartScreen Warning:** Depending on your build or certificate status, Windows SmartScreen may flag the executable on its first run. Select "More info" → "Run anyway".

---

## Technical Details

<details>
<summary>Expand for root cause and implementation specifics</summary>

### Root cause

The GoXLR exposes multiple virtual audio endpoints to Windows via USB. Windows implements an **Audio Device Class Inactivity Timer** that transitions idle audio endpoints to a low-power state (D3).

When an application like OBS holds a WASAPI shared-mode session on one of these endpoints and the endpoint transitions to D3, the session handle becomes invalidated (`AUDCLNT_E_DEVICE_INVALIDATED`). OBS does not gracefully recover:
1. The `IAudioClient` session handle goes stale.
2. Volume control calls (`SetVolume`, `SetChannelVolume`) silently fail.
3. Audio data pushed to the endpoint buffer arrives corrupted or desynchronized.
4. The OBS audio thread eventually deadlocks waiting for a response from the invalidated session.

### Implementation

The tool opens `IAudioClient` sessions in `AUDCLNT_SHAREMODE_SHARED` mode on every GoXLR render endpoint and calls `IAudioRenderClient::GetBuffer` / `ReleaseBuffer` with `AUDCLNT_BUFFERFLAGS_SILENT`. This keeps the endpoint's audio engine active.

A health check runs every 5 seconds, calling `IAudioClient::GetCurrentPadding()` to detect invalidated sessions and automatically restarts them.

Key details:
* `AUDCLNT_STREAMFLAGS_NOPERSIST` — the silence sessions don't appear in the Windows Volume Mixer.
* Fallback to `IAudioClient::GetMixFormat()` if the endpoint rejects the default 48kHz/16bit/stereo format.
* COM initialized as STA (apartment-threaded) for shell notification icon compatibility.
* Hidden `HWND_MESSAGE` window for the Win32 message pump.
* Programmatic tray icon generation (no `.ico` resource needed).

### References
* [Audio Device Class Inactivity Timer (Microsoft)](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/audio-device-class-inactivity-timer-implementation)
* [WASAPI Stream Management (Microsoft)](https://learn.microsoft.com/en-us/windows/win32/coreaudio/stream-management)
* [Immediate Idle Timeout Opt-in (Microsoft)](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/immediate-idle-timeout-opt-in)
* [GoXLR Utility](https://github.com/GoXLR-on-Linux/goxlr-utility)

</details>

## License

See the [LICENSE](LICENSE) file.

## Credits

Bug diagnosis and development by [Colin Brigato](https://github.com/cjbrigato). Thanks to the streaming community for assisting in reproducing and diagnosing the WASAPI session invalidation behavior.
