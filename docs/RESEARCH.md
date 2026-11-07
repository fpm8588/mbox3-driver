# Getting an Avid Mbox 3 working on macOS 27 Golden Gate (Apple Silicon) for $0

Research date: 7 November 2026. Every claim below was checked against a primary source (Linux kernel source,
Apple developer documentation, Apple developer forums, the source code of two working open-source drivers) or
against the hardware report from your own Mac. Sources are listed at the end.

---

## 1. Your hardware

From System Information on your Mac:

| Field | Value | What it means |
|---|---|---|
| USB Vendor ID | `0x0dba` | Digidesign (Avid) |
| USB Product ID | `0x5000` | Mbox, 3rd generation (the same ID the Linux kernel and GhostBox use) |
| USB Product Version | `0x0108` | **Firmware 1.08**, the same firmware the GhostBox Revenant author verified the protocol on |
| Link speed | 480 Mb/s | USB 2.0 high speed (required for 4×4 channels at up to 96 kHz). Good. |
| Power allocated | 498 mA | Bus-powered, within the USB 2.0 budget |
| Bus | USB 3.1 Bus, Removable | Plugged directly into the Mac, not through a hub. Keep it that way. |

The macOS USB stack sees it fine. What's missing is something that turns it into a Core Audio device.

## 2. Why it stopped working

1. **The Mbox hides its audio interfaces behind vendor class codes (`0xFF`).** Inside, it speaks USB Audio
   Class 2, but because it labels itself vendor-specific, Apple's built-in USB audio driver never claims it.
   It needs a vendor driver.
2. **Avid's last Mac driver (1.2.13, March 2018, for High Sierra) is a kernel extension with 32-bit parts.**
   macOS hasn't run 32-bit code since Catalina, and Apple Silicon Macs can't load Intel kernel extensions.
3. **Drivers can't run through Rosetta**, even when apps can. On top of that, Golden Gate is the last macOS
   with Rosetta at all, so there's no Intel-compatibility future to wait for.
4. **Avid has never shipped an Apple Silicon driver for the 3rd-generation Mbox**, and it isn't going to for
   hardware from 2010.

The good news: everything a driver needs is publicly documented, and two independent working implementations
exist.

## 3. Every option, compared

| # | Option | Cost | macOS stays fully secure? | Effort | Verdict |
|---|---|---|---|---|---|
| A | **Install GhostBox Revenant** (existing open-source Mbox 3 driver) | $0 | Yes (no kext, no SIP change) | Minutes | **Fastest way to get sound today** |
| B | **Build your own AudioServerPlugIn that does the USB work itself** | $0 | Yes | Weeks | **What this repo does** |
| C | AudioServerPlugIn + a separate user-space USB engine (shared memory) | $0 | Yes | Weeks+ | Works (the Eleven Rack driver uses it); more moving parts |
| D | DriverKit / AudioDriverKit system extension | $99/yr in practice | Yes once signed | Weeks | Not $0 (see below) |
| E | Kernel extension (or a "codeless kext" pointing Apple's driver at it) | $0 | **No**, needs Reduced Security | Weeks | Dead end |
| F | Linux VM (UTM) + USB passthrough + Linux's built-in Mbox 3 support + network audio back to macOS | $0 | Yes | Days | Works for experiments; too much latency for tracking |
| G | Asahi Linux dual boot | $0 | n/a (it's Linux) | Hours | Only some Apple Silicon Macs; it's not macOS |
| H | Use the Mbox as a standalone preamp/monitor box with `mbox3ctl` | $0 | Yes | Minutes | Partial: hardware mixing only, no recording |
| I | Old Intel Mac on High Sierra with Avid's driver | Money unless you own one | n/a | Hours | Not $0 |
| J | Replace it with a class-compliant interface | Money | Yes | Minutes | Not $0 |

### A. GhostBox Revenant: an existing driver

- `github.com/isaacmateosv/ghostbox-revenant`, **GPL-3.0-or-later**, version 1.0.0 release candidate
  (rc.1 published 29 Oct 2026).
- A user-space AudioServerPlugIn that streams isochronous USB itself, plus a Swift control app that recreates
  Avid's control panel (hardware mixer, reverb/delay, tuner, meters, presets). 4 in / 4 out at 44.1–96 kHz.
- Its status page says it's been used for audio in and out at 48 kHz in Logic **on Apple Silicon with macOS 27**,
  on one Mac with a real Mbox 3. Several things are still listed as pending: hardware tests of the restructured
  driver, the low-latency modes, S/PDIF, and a second Mac. It's not signed with an Apple Developer ID, so you
  approve it once in System Settings → Privacy & Security → **Open Anyway**.
- Known issue: restarting Core Audio while audio is playing can leave the Mbox silent until you replug it.
- **If you only want sound, install this.** If you build your own as well, don't run both drivers at once:
  two plug-ins fighting over the same streaming interfaces won't work.

### B. Your own user-space AudioServerPlugIn (chosen design)

An **AudioServerPlugIn** (also called a HAL plug-in) is a `.driver` bundle in `/Library/Audio/Plug-Ins/HAL`
that Core Audio's daemon (`coreaudiod`) loads into a sandboxed helper process. It isn't a kernel extension
and needs no DriverKit entitlement. Apple publishes a sample of one ("Creating an Audio Server Driver Plug-in",
the NullAudio sample).

The catch was always the sandbox: the plug-in's host process blocks most IOKit access, and Apple's mailing-list
archives have developers hitting `deny iokit-open IOUSBDeviceUserClient` in exactly this situation. The fix is
declaring the USB user-client classes in the plug-in's Info.plist. Apple's header documents this mechanism for
custom user clients, and GhostBox ships this exact configuration on macOS 27:

```xml
<key>AudioServerPlugIn_IOKitUserClientClasses</key>
<array>
    <string>AppleUSBHostDeviceUserClient</string>
    <string>AppleUSBHostInterfaceUserClient</string>
    <string>IOUSBDeviceUserClientV2</string>
    <string>IOUSBInterfaceUserClientV3</string>
</array>
```

With that in place, the plug-in can use the ordinary IOKit USB API (`IOUSBLib`: `DeviceRequestTO` for control,
`LowLatencyReadIsochPipeAsync` / `LowLatencyWriteIsochPipeAsync` for audio). That API is formally deprecated but
still works on Apple Silicon; both reference drivers rely on it.

**Why this design over C:** there's one process and no inter-process audio hop. Every buffer the HAL asks for is
filled from a ring the USB callbacks write directly. GhostBox tried design C first and got no sound, then got
clean audio with B. (Eleven Rack proves C *can* work, so it's a fallback, not impossible.)

**What it costs:** $0. The free Xcode Command Line Tools build it, ad-hoc signing (`codesign --sign -`) is
enough for a HAL plug-in you install yourself, and GitHub Actions' Apple Silicon runners are free and
unmetered for public repositories. Without Apple's $99 Developer ID there's no notarization, which only means
clicking **Open Anyway** once when you install a package you downloaded. Things you build locally aren't
quarantined, so they don't even trigger that.

### C. Plug-in + separate engine

The Avid **Eleven Rack** driver (`github.com/Matt-Housley/eleven-rack-driver`, MIT) does this: a Core Audio
plug-in, a user-space engine that owns USB, and a menu bar app, with audio passed through a POSIX
shared-memory ring. It's a valid fallback if B ever hits a sandbox wall on a future macOS. Because it's MIT,
it's also a reference you can borrow code from without changing your license.

### D. DriverKit / AudioDriverKit: why it isn't $0

AudioDriverKit (macOS 12+) is Apple's modern framework for hardware audio drivers, and it's what Apple would
recommend. But a dext needs `com.apple.developer.driverkit` and a USB transport entitlement in a provisioning
profile. An Apple DTS engineer confirmed on the developer forums that local DriverKit development works without
an approval process, but users report that the development provisioning profiles require a **paid** account.
The no-paid-account path means disabling System Integrity Protection, which isn't acceptable on a production
Mac. (The R11 Eleven Rack driver went this way and is Apple-signed, which shows it can be done, with an account.)
It can be a later upgrade if you ever pay for the program.

### E. Kernel extensions: dead end

GhostBox's logbook documents a failed "codeless kext" attempt. Apple's USB audio driver couldn't start the
streams, it grabbed the interfaces exclusively, and it can't handle the Mbox's quirks: implicit feedback, the
S/PDIF clock at power-up, and the silent default mixer. Any kext on Apple Silicon also needs Reduced Security
set from Recovery.

### F/G. Linux routes

Linux has supported the Mbox 3 since kernel 6.1 (2022 quirk by Conner Knox, extended to 44.1–96 kHz by Manuel
Barrio Linares). A UTM virtual machine with USB passthrough gets it working in Linux, but getting audio back
into macOS means network audio (JACK/PipeWire network sinks) and real latency. Asahi Linux runs natively on some
Apple Silicon Macs (the M1/M2 generation; check asahilinux.org for your model) with full ALSA support, but then
you're in Linux, not macOS with Pro Tools/Logic.

### H. Standalone mode with `mbox3ctl` (free bonus from Phase 1)

The Mbox has a zero-latency hardware mixer, but it powers up **silent** and on the **S/PDIF clock**. Something
has to initialize it over USB. `mbox3ctl init` followed by `mbox3ctl monitor on` routes In 1/2 straight to the
outputs and headphones, which makes it a working preamp/headphone box with no driver at all. You can't record
on the Mac this way, though.

## 4. The protocol (what a driver has to do)

Verified from two independent sources that agree: the Linux kernel (`sound/usb/quirks.c`, `quirks-table.h`)
and GhostBox's `docs/PROTOCOL.md` (verified on firmware 1.08, your firmware). Encoded in
`Sources/Shared/Mbox3Protocol.h`.

| Topic | Fact |
|---|---|
| Interfaces | 1 = control, 2 = playback (alt 1, EP `0x01` isoch OUT), 3 = capture (alt 1, EP `0x81` isoch IN), 4 = class-compliant MIDI (macOS already handles it) |
| Format | 4 channels, 24-bit little-endian packed (S24_3LE), 12 bytes/frame, max 156 bytes/packet (13 frames, enough for 96 kHz) |
| Rates | 44.1, 48, 88.2, 96 kHz |
| Timing | Asynchronous with **implicit feedback**: each OUT packet carries as many frames as the matching IN packet. One packet per 125 µs microframe |
| OUT layout | Packets inside one transfer are **packed back to back**. Fixed 156-byte slots garble the audio |
| Clock | Selector `0x8001` (1 = internal, 2 = S/PDIF), internal rate `0x8101` (4 bytes LE Hz). **Powers up on S/PDIF and won't stream without an S/PDIF signal → select internal first** |
| Mixer | Unit `0x40`: `wValue = 0x0100 | (input*4 + output)`, int16 LE in 1/256 dB, `0x8000` = off. **Write-only** and **silent at power-up** |
| Control requests | `bmRequestType 0x21` set / `0xA1` get, `bRequest 0x01` (CUR) or `0x03` (MEM, extension unit), `wIndex = unit<<8 | 1` |
| Not on USB | Phantom power, gain, pad, Soft Limit, front/rear switches are analog-only. No software can switch them, Avid's included |

## 5. Hard-won lessons from the existing drivers (avoid repeating them)

From GhostBox's logbook and Eleven Rack's notes. Each one cost someone days:

1. Schedule OUT isochronous transfers **5–10 ms ahead** of the bus frame, or the host controller rejects them.
2. USB completions can arrive up to **~7 ms late**, so the HAL safety offsets must cover that or you'll hear clicks.
3. In `coreaudiod`, **anything that changes by itself** (a property you keep updating, adding and removing
   devices on hot plug) can spin the audio daemon at 100% CPU and freeze Audio MIDI Setup. Keep one static device
   and flip `kAudioDevicePropertyDeviceIsAlive` on plug/unplug.
4. **Core Audio owns the sample rate.** Never push a hardware rate change back to the HAL (it loops forever),
   except when following an external S/PDIF clock.
5. After `coreaudiod` is killed mid-stream, the Mbox may ignore playback until it's re-enumerated. Select
   alt 0 before alt 1 when starting, and do a software replug after installs.
6. The mixer can't be read back, so whatever writes it has to remember and restore state on every attach.
7. A broken HAL plug-in can take down **all** Mac audio. Always know the recovery command (see PROMPT.md).

## 6. Licensing

- Linux's Mbox 3 code is GPL-2.0 and GhostBox is GPL-3.0. **Protocol facts aren't copyrightable**, so using them
  (as this repo does) is fine under any license. **Copying their code** would make this project GPL. This repo
  is MIT and contains only original code. If you'd rather reuse GhostBox code directly, switch to GPL-3.0 first.
- Eleven Rack (MIT) and Apple's NullAudio sample (Apple sample-code license, permissive) can be borrowed from
  with attribution.
- Not affiliated with Avid. "Avid" and "Mbox" are Avid's trademarks, used only to name the hardware.

## 7. Sources

- Linux kernel `sound/usb/quirks.c` and `quirks-table.h` (Mbox 3 boot quirk, defaults, rate setting):
  github.com/torvalds/linux/tree/master/sound/usb. Patch history: lore.kernel.org/alsa-devel (Conner Knox, 2022);
  sample-rate support by Manuel Barrio Linares.
- GhostBox Revenant: github.com/isaacmateosv/ghostbox-revenant (`README.md`, `docs/STATUS.md`,
  `docs/ARCHITECTURE.md`, `docs/PROTOCOL.md`, `docs/LOGBOOK.md`, `docs/INSTALL.md`, `build.sh`).
- Eleven Rack Driver: github.com/Matt-Housley/eleven-rack-driver. Coverage: Production Expert, "Eleven Rack Is Back!
  Independent Developer Releases Apple Silicon Driver" and "R11 Compatibility Driver Brings Avid Eleven Rack
  Support To Apple Silicon Macs".
- Apple: `AudioServerPlugIn.h` (sandbox and user-client Info.plist keys); sample code "Creating an Audio Server
  Driver Plug-in" and "Building an Audio Server Plug-in and Driver Extension"; WWDC21 "Create audio drivers with
  DriverKit"; "Requesting Entitlements for DriverKit Development".
- Apple Developer Forums: "Develop DriverKit extensions locally with a Free Developer Account" (thread 847647),
  "Local DriverKit development blocked by provisioning profile requirement" (thread 776657).
- coreaudio-api mailing list, Dec 2015: sandbox `deny iokit-open IOUSBDeviceUserClient_V2` from a HAL plug-in.
- GitHub Docs, "About billing for GitHub Actions": standard hosted runners are free for public repositories; macOS
  minutes count 10× against private-repo quotas.
- macOS 27 Golden Gate: MacRumors (announced 8 June 2026), Low End Mac (first Apple-Silicon-only macOS);
  gori.me (last release with Rosetta 2).
- Production Expert, "Pro Tools HDX and video formats tested on Apple M1 Max": drivers can't run through Rosetta.
