# Build prompt: Mbox3Driver (paste this into Claude Code on the Mac, from the repo root)

---

You're continuing **Mbox3Driver**, a free, open-source macOS driver that makes the **Avid Mbox (3rd
generation), USB `0dba:5000`, firmware 1.08** a normal Core Audio device again on an **Apple Silicon Mac running
macOS 27 Golden Gate**. Read `docs/RESEARCH.md` first (why this design, the protocol, lessons from prior drivers),
then `Sources/Shared/Mbox3Protocol.h` (the single source of truth for every hardware value).

## Hard constraints

1. **$0.** Only the free Xcode Command Line Tools (or free Xcode), ad-hoc signing (`codesign --sign -`), free
   GitHub Actions on this public repo. No paid Apple Developer account, no DriverKit, no kernel extensions,
   **never ask to disable SIP or lower boot security.**
2. **Apple Silicon (arm64), macOS 13+ deployment target, tested on macOS 27.** No Intel/Rosetta work needed.
3. **Original code, MIT license.** You may *read* GhostBox Revenant (GPL-3) and the Linux kernel (GPL-2) for
   protocol facts, but don't copy their code. Apple's NullAudio sample ("Creating an Audio Server Driver
   Plug-in") and the Eleven Rack driver (MIT) may be adapted with attribution in `NOTICE.md`.
4. **One source of truth.** Every USB constant comes from `Sources/Shared/Mbox3Protocol.h`. If you learn
   something new from the hardware, update that header (with a comment saying how it was verified) and its tests.
5. **Hardware-free tests for everything that can be tested without hardware.** `./build.sh test` must pass on
   Linux and macOS, and CI (`.github/workflows/ci.yml`) must be green before a phase counts as done.
6. **Never leave the Mac without sound.** Every install path has an uninstall path, and every session starts by
   reminding the user of the recovery command:
   ```bash
   sudo rm -rf /Library/Audio/Plug-Ins/HAL/Mbox3Driver.driver && sudo killall coreaudiod
   ```
7. **Small, verified steps.** At each hardware checkpoint, stop, tell the user exactly what to run and what to
   listen or look for, and wait for the result. Don't guess at hardware behavior.

## Repository layout (keep it)

```
Sources/Shared/        Mbox3Protocol.h, Mbox3Codec.h, Mbox3Defaults.h — pure C, no OS headers
Sources/mbox3ctl/      Phase 1 CLI (IOKit) + portable descriptor printer
Sources/Driver/        Phase 2-3 AudioServerPlugIn (C or C++17), Info.plist.in
Sources/App/           Phase 4 SwiftUI menu bar app
Installer/             Phase 5 pkg scripts, uninstaller
Tests/                 hardware-free unit tests
docs/                  RESEARCH.md, PROMPT.md, HARDWARE_LOG.md (create it: record every hardware result)
build.sh               test | cli | driver | install-driver | uninstall-driver | app | pkg | all
```

## Phase 1: hardware access (code written; needs a run on the real Mbox)

`mbox3ctl` is built. Have the user run these on the Mac with the Mbox plugged in directly (no hub), and record
the output in `docs/HARDWARE_LOG.md`:

| Step | Command | Expected |
|---|---|---|
| 1 | `./build.sh cli` | builds an arm64 `build/mbox3ctl` |
| 2 | `build/mbox3ctl info` | 0dba:5000, firmware 1.08, high speed, wTotalLength 464, interfaces 1–4 vendor class 0xFF, EP 0x01 OUT / 0x81 IN isoch async, wMaxPacketSize 156, bInterval 1 |
| 3 | `build/mbox3ctl clock` | probably `S/PDIF` at power-up |
| 4 | `build/mbox3ctl init` | 54 writes, 0 failed |
| 5 | `build/mbox3ctl clock` | internal, 48000 (or whatever it was) |
| 6 | `build/mbox3ctl rate 44100`, then `96000`, then `48000` | "OK" each time |
| 7 | `build/mbox3ctl meters --watch` while speaking into a mic on In 1 | the "In 1" bar moves; In 2 stays low |
| 8 | `build/mbox3ctl monitor on` with headphones on | you hear the mic live (hardware mixer works) |
| 9 | `build/mbox3ctl mono on` / `dim on` / `tuner on` (then off) | front-panel LEDs react |
| 10 | `build/mbox3ctl replug` | Mbox disappears and comes back |

If step 4 reports failures on the tuner write, retry it with `bRequest 0x01`
(`build/mbox3ctl raw set 1 0x0003 0x2001 0`) and record which form works. Phase 1 is done when steps 2–8 pass.

## Phase 2: a HAL plug-in that publishes the device (no USB yet)

Goal: **"Mbox 3" shows up in Audio MIDI Setup** with 4 inputs and 4 outputs, rates 44.1/48/88.2/96 kHz,
playing silence on a host-time clock. This de-risks all the Core Audio object-model work separately from USB.

- Start from the structure of Apple's NullAudio sample (C, `AudioServerPlugInDriverInterface`), written fresh
  for this project. Objects: plug-in (1), device (2), input stream (3), output stream (4). No volume controls
  (the Mbox has no software-controllable analog gain).
- Factory function `Mbox3Driver_Create`, factory UUID and keys exactly as in `Sources/Driver/Info.plist.in`
  (including `AudioServerPlugIn_IOKitUserClientClasses`, needed in Phase 3).
- Physical format: 4 ch, 32-bit float, packed, interleaved; virtual format the same.
- Channel names: In 1, In 2, S/PDIF In L, S/PDIF In R / Return 1, Return 2, Return 3, Return 4.
- Device UID `Mbox3Driver:0dba:5000`, model UID `Mbox3Driver:model`, transport `kAudioDeviceTransportTypeUSB`,
  `kAudioDevicePropertyDeviceCanBeDefaultDevice` = true for both scopes.
- **The device object always exists.** In Phase 3 hot plug only flips `kAudioDevicePropertyDeviceIsAlive`.
  Never add or remove devices at runtime (it spins coreaudiod).
- Never call the client HAL API (`AudioObjectGetPropertyData` etc.) from inside the plug-in.
- `build.sh driver` builds `build/Mbox3Driver.driver` (arm64, `-mmacosx-version-min=13.0`, ad-hoc signed).
  `build.sh install-driver` copies it to `/Library/Audio/Plug-Ins/HAL`, runs `sudo killall coreaudiod`, then
  checks for 60 seconds that `coreaudiod` CPU stays under 20%, **auto-uninstalling if it doesn't**.
  `build.sh uninstall-driver` reverses it.
- Unit test the object model in-process: a test harness that `dlopen`s the bundle's binary, calls the factory,
  `Initialize`s it with a fake host, and reads every property of every object (no coreaudiod needed). Add this to CI.
- **Hardware checkpoint:** the user installs it, opens Audio MIDI Setup, and sees "Mbox 3" with 4×4 at every rate,
  and coreaudiod stays calm. Then uninstall before Phase 3 if anything is off.

## Phase 3: isochronous streaming inside the plug-in

Goal: clean record and playback at all four rates in Logic/Pro Tools/GarageBand.

Design (proven by prior drivers; see RESEARCH.md §5):
- **USB thread** with its own CFRunLoop: `IOServiceAddMatchingNotification` for `0dba:5000` arrival and
  termination; on arrival open the device (`IOUSBDeviceInterface320`) and interfaces 2 and 3
  (`IOUSBInterfaceInterface942`), `USBInterfaceOpen`, **select alt 0 then alt 1**, find the pipes for EP 0x01 and
  0x81, add the async event sources to the run loop, write the default state (`Mbox3Defaults.h`), set the rate
  Core Audio wants, select the internal clock.
- **IN** (capture) paces everything: keep ~16 transfers of 8 microframes (1 ms each) queued with
  `LowLatencyReadIsochPipeAsync`, request `MBOX3_MAX_PACKET` per microframe, buffers from `LowLatencyCreateBuffer`.
  On each completion: parse with `mbox3_parse_in_transfer` (slot stride = request size), push frames into the
  input ring, record each packet's frame count, update the clock model, then top up OUT.
- **OUT** (playback): frame counts copied from IN (implicit feedback; `Mbox3PacketPacer` only before the first
  IN completes), built with `mbox3_build_out_transfer` (**packed back to back**), scheduled **5–10 ms ahead** of
  `GetBusFrameNumberWithTime`, never behind frames already queued, via `LowLatencyWriteIsochPipeAsync`.
- **Rings:** lock-free single-producer/single-consumer, indexed by device sample time. The HAL reads input frame
  *n* and writes output frame *n* at sample time *n*. No locks, allocations, logging or Objective-C on the
  IO thread or the USB completion path.
- **Clock / `GetZeroTimeStamp`:** derive sample time ↔ host time from the IN completions' microframe timestamps
  (`frTimeStamp`), smoothed (e.g. a linear fit over the last ~100 ms). Period = ring size (e.g. 16384 frames).
- **Safety offsets** start conservative: 10 ms input / 13 ms output (USB completions can be ~7 ms late). Make
  them a "latency mode" later (Safe / Balanced / Low), stored in plug-in storage. Report
  `kAudioDevicePropertyLatency` and stream latency honestly.
- **Sample rate:** Core Audio is the only authority. On `kAudioDevicePropertyNominalSampleRate` change, request a
  config change, stop streams, write `0x8101` + select internal on `0x8001`, read back, restart. Never push a
  hardware-side rate change into the HAL (except a future S/PDIF-follow mode).
- **Hot plug:** unplug → stop IO cleanly, `DeviceIsAlive = false`; replug → reopen, restore, `DeviceIsAlive = true`.
- Pure logic (ring math, clock fit, packet scheduling decisions) goes in `Sources/Shared` or a testable C++
  header with unit tests that run in CI.
- **Hardware checkpoints:** (a) record 30 s of speech at 48 kHz in GarageBand, which must be clean; (b) play a
  1 kHz sine for 5 min, no clicks; (c) round-trip latency measured in a DAW; (d) repeat at 44.1/88.2/96;
  (e) unplug/replug while the DAW is open; (f) `sudo killall coreaudiod` mid-playback, then recover.

## Phase 4: control app

A SwiftUI menu bar app (`Sources/App`, built with `swiftc`/SwiftPM from `build.sh app`, ad-hoc signed) that
talks to the hardware through control requests directly (no exclusive access, so it coexists with the driver):
the 36-crosspoint monitor mixer for both stereo mixes (faders, pan, mute, solo, link), FX sends, reverb/delay
preset/duration/feedback/return, Mono, Dim, tuner (pitch arrives as MIDI Poly Aftertouch on the Mbox's Control
MIDI port), live meters (~15 Hz), clock source display. **The mixer is write-only:** persist the state in
`~/Library/Application Support/Mbox3Driver/` and restore it on every attach. Put the mixer math in a pure Swift
module with unit tests.

## Phase 5: packaging

`build.sh pkg` → `Mbox3Driver-<version>.pkg` via `pkgbuild` + `productbuild` (app to `/Applications`, driver to
`/Library/Audio/Plug-Ins/HAL`, postinstall restarts coreaudiod with the 60-second CPU watchdog), an
`Uninstall.command`, and a GitHub Release workflow on tags. Document the one-time **Open Anyway** step for
unnotarized packages in the README.

## Working style

- Before each phase, restate the plan in a few lines. After each phase, update `README.md` status,
  `docs/HARDWARE_LOG.md`, and commit with a clear message.
- When the hardware disagrees with the docs, the hardware wins: update the header, the tests and RESEARCH.md.
- Run `./build.sh test` before every commit; push and check that CI is green.
- Debug with: `log stream --predicate 'process == "coreaudiod" OR subsystem CONTAINS "Mbox3"'`,
  `log show --last 5m --predicate 'eventMessage CONTAINS "deny"'` (sandbox denials), Audio MIDI Setup,
  `top -o cpu` for coreaudiod.
