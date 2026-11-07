# Mbox3Driver

A free, open-source driver that brings the **Avid Mbox (3rd generation)** (USB `0dba:5000`) back to life on
**Apple Silicon Macs running macOS 27 Golden Gate**, as a normal Core Audio device for Logic, Pro Tools, GarageBand
and system audio.

- **$0 to build and use:** free Xcode Command Line Tools, ad-hoc signing, free GitHub Actions.
- **Runs entirely in user space:** an AudioServerPlugIn plus IOKit USB. No kernel extension, no DriverKit, and
  no SIP or security changes.
- **MIT licensed, original code.** Protocol facts come from the Linux kernel's Mbox 3 support and GhostBox
  Revenant's documentation (see [docs/RESEARCH.md](docs/RESEARCH.md)).

> Just want sound right now? [GhostBox Revenant](https://github.com/isaacmateosv/ghostbox-revenant) (GPL-3) is an
> existing driver for the same hardware. Don't install both at once.

## Status

| Phase | What | State |
|---|---|---|
| 1 | `mbox3ctl`: talk to the Mbox from the command line (info, clock, rate, init, meters, mixer) | Code done, CI build; **needs a run on the real Mbox** |
| 2 | HAL plug-in publishes "Mbox 3" (4 in / 4 out) in Audio MIDI Setup | Next |
| 3 | Isochronous record/playback inside the plug-in | Planned |
| 4 | Menu bar control app (hardware mixer, FX, meters, tuner) | Planned |
| 5 | Installer package | Planned |

The full plan and the prompt to continue in Claude Code are in [docs/PROMPT.md](docs/PROMPT.md).

## Try Phase 1

```bash
xcode-select --install          # once, free
git clone https://github.com/fpm8588/mbox3-driver.git && cd mbox3-driver
./build.sh test                 # hardware-free unit tests
./build.sh cli                  # builds build/mbox3ctl
build/mbox3ctl info             # descriptors, firmware, speed
build/mbox3ctl init             # internal clock + default mixer (the Mbox powers up silent, on S/PDIF)
build/mbox3ctl meters --watch   # live hardware meters, Ctrl-C to stop
build/mbox3ctl monitor on       # hear In 1/2 in the headphones through the hardware mixer
```

If you download `mbox3ctl` from a CI run instead of building it, clear the download quarantine first:
`xattr -d com.apple.quarantine mbox3ctl && chmod +x mbox3ctl`.

## Recovery

If a future driver build ever breaks your Mac's audio:

```bash
sudo rm -rf /Library/Audio/Plug-Ins/HAL/Mbox3Driver.driver && sudo killall coreaudiod
```

Not affiliated with Avid. Avid and Mbox are trademarks of Avid Technology, Inc., used only to name the hardware.
