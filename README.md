# GoXLR Keep-Alive 🔇⚡

**Fixes the crackling/buzzing + OBS freeze bug on GoXLR and GoXLR Mini.**

A tiny system tray app that runs silently in the background. No setup, no config, no audio knowledge required. Just run it and forget about it.

---

## Is this for me?

**You have this bug if ALL of these sound familiar:**

1. You're streaming or recording with OBS + a GoXLR (or GoXLR Mini)
2. After a while, one of your audio channels starts **crackling, buzzing, or producing distorted/saturated audio** — usually a channel you haven't used in a while (like Sound Alerts, Music, or a game channel you just switched to)
3. When you try to **lower the volume of that channel in OBS, nothing happens** — the fader moves but the audio doesn't change
4. Within seconds, OBS **freezes completely** (spinning wheel / "Not Responding") and the only fix is to force-close and restart OBS
5. After restarting OBS, everything works fine again... **until it happens again** days or weeks later
6. You may have noticed that **resetting your Windows sound device assignments** (Settings → Sound → Volume Mixer, reassigning which app goes where) "fixes" it for a few weeks

**Sound familiar? That's exactly the bug this tool fixes.**

You are not alone. This has been reported across GoXLR Discord servers, OBS forums, and GitHub issues for years. TC-Helicon never fixed it (and they won't — the entire team has been laid off). The official GoXLR app is abandoned.

---

## What's actually going on? (The short version)

Windows has a power-saving feature that puts audio channels to sleep when they haven't been used for a while. When the channel wakes back up, OBS gets confused because its connection to that channel has gone stale. OBS can't control the volume anymore, the audio comes through corrupted, and eventually OBS locks up trying to talk to a channel that isn't listening.

**GoXLR Keep-Alive prevents this by keeping every GoXLR channel gently awake at all times.** It sends inaudible digital silence to each channel so Windows never puts them to sleep. Zero impact on your audio, zero CPU usage, zero configuration.

---

## Download & Install

### Option 1: Pre-built release (recommended)
1. Go to the [Releases](../../releases) page
2. Download `GoXLRKeepAlive.exe`
3. Run it — a small green dot appears in your system tray (bottom-right, near the clock)
4. That's it. It's working.

### Option 2: Build from source
1. Open `GoXLRKeepAlive.sln` in Visual Studio 2022
2. Select **Release | x64**
3. Build (Ctrl+Shift+B)
4. Your exe is in `bin\Release\`

> **Note:** You need the "Desktop development with C++" workload installed in Visual Studio. No other dependencies. No runtime to install. No frameworks. Just C++ and Windows.

---

## Auto-start at login

You probably want this to run automatically every time you turn on your PC.

**Easy way:** Press `Win+R`, type `shell:startup`, press Enter. Drop a shortcut to `GoXLRKeepAlive.exe` in that folder. Done.

**PowerShell way:**
```powershell
$s = (New-Object -ComObject WScript.Shell).CreateShortcut("$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Startup\GoXLRKeepAlive.lnk")
$s.TargetPath = "C:\path\to\GoXLRKeepAlive.exe"
$s.Save()
```

---

## System tray menu

Right-click (or left-click) the green dot in your system tray:

| Option | What it does |
|--------|-------------|
| **3/3 endpoints alive** | Shows how many GoXLR channels are being kept awake |
| **Show Details** | Lists every GoXLR endpoint and its status |
| **Restart Sessions** | Re-scans for GoXLR devices (use after USB replug) |
| **Quit** | Stops the keep-alive and exits |

The dot is **green** when all endpoints are alive, **red** if something went wrong.

---

## FAQ

**Will this add any sound to my stream/recording?**
No. It sends digital silence — literally zeros. Mixing silence with your actual audio changes nothing. `0 + your audio = your audio`.

**Does it use CPU/RAM?**
Essentially nothing. The app sleeps 99.99% of the time. It's a ~150 KB executable with no background processing.

**Does it work with GoXLR Utility (the open-source app)?**
Yes. It works with both the official (abandoned) GoXLR app and the community GoXLR Utility. It doesn't care which app manages your GoXLR — it only talks to the Windows audio endpoints.

**What if I unplug and replug my GoXLR?**
The app will detect the lost sessions within 5 seconds and try to reconnect. If that doesn't work, right-click the tray icon → Restart Sessions.

**Do I still need to do the other fixes? (USB power management, etc.)**
They help and are recommended, but this tool alone should prevent the crash. For maximum reliability, also do these:
- Disable **USB Selective Suspend** (Power Options → Advanced → USB)
- Disable **"Allow the computer to turn off this device"** on USB Root Hubs (Device Manager)
- Make sure all GoXLR endpoints are set to the **same sample rate** (48000 Hz)

**I don't have this exact bug, but my GoXLR audio crackles randomly.**
This tool specifically targets the idle-channel-wakeup bug. Random crackling can also be caused by USB bandwidth conflicts (too many USB devices on the same controller), ground loops, or driver issues. This tool won't hurt in those cases, but it might not help either.

**Will Windows Defender / SmartScreen flag this?**
~The release builds are signed with an Authenticode certificate. You should see "Open Source Developer, Colin Brigato" as the publisher~ (This is ongoing, hopefully). If you build from source yourself, your build won't be signed and SmartScreen may show a warning on first run — click "More info" → "Run anyway".

---

## The technical details (for the curious)

<details>
<summary>Click to expand — you don't need this to use the tool</summary>

### Root cause

The GoXLR (and GoXLR Mini) exposes multiple virtual audio endpoints to Windows via USB (Chat Mic, Broadcast Stream Mix, Music, Game, System, etc.). Windows implements an **Audio Device Class Inactivity Timer** that transitions idle audio endpoints to a low-power state (D3).

When an application like OBS holds a WASAPI shared-mode session on one of these endpoints and the endpoint transitions to D3 due to inactivity, the session handle becomes **invalidated** (`AUDCLNT_E_DEVICE_INVALIDATED`). OBS does not gracefully recover from this condition:

1. The `IAudioClient` session handle goes stale
2. Volume control calls (`SetVolume`, `SetChannelVolume`) silently fail — explaining why the fader stops working
3. Audio data pushed to the endpoint buffer arrives corrupted or desynchronized — causing the crackling
4. The OBS audio thread eventually deadlocks waiting for a response from the invalidated session — causing the freeze

### Why resetting Windows sound assignments "fixes" it

When you go to Windows Settings → Sound → Volume Mixer and reassign device mappings, Windows destroys all existing audio sessions and forces applications to create new ones. OBS gets fresh, valid WASAPI session handles. The fix persists until another channel sits idle long enough to trigger the inactivity timer again.

### What this tool does

Opens `IAudioClient` sessions in `AUDCLNT_SHAREMODE_SHARED` mode on every GoXLR render endpoint and calls `IAudioRenderClient::GetBuffer` / `ReleaseBuffer` with `AUDCLNT_BUFFERFLAGS_SILENT`. This keeps the endpoint's audio engine active without producing any audible output.

A health check runs every 5 seconds, calling `IAudioClient::GetCurrentPadding()` to detect invalidated sessions and automatically restarting them.

Key implementation details:
- `AUDCLNT_STREAMFLAGS_NOPERSIST` — the silence sessions don't appear in the Windows Volume Mixer
- Fallback to `IAudioClient::GetMixFormat()` if the endpoint rejects our default 48kHz/16bit/stereo format
- COM initialized as STA (apartment-threaded) for shell notification icon compatibility
- Hidden `HWND_MESSAGE` window for the Win32 message pump
- Programmatic tray icon generation (no .ico resource needed)

### References
- [Audio Device Class Inactivity Timer (Microsoft)](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/audio-device-class-inactivity-timer-implementation)
- [WASAPI Stream Management (Microsoft)](https://learn.microsoft.com/en-us/windows/win32/coreaudio/stream-management)
- [Immediate Idle Timeout Opt-in (Microsoft)](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/immediate-idle-timeout-opt-in)
- [GoXLR Utility (community replacement app)](https://github.com/GoXLR-on-Linux/goxlr-utility)

</details>

---

## License

See LICENSE file

## Credits

Bug diagnosis and tool by [Colin Brigato](https://github.com/cjbrigato).
The critical clue came from a streamer who said: *"I turned the volume down but it didn't go down."*
That one sentence cracked the whole thing.
