# Mbox3Driver: notes for Claude Code

Read `docs/PROMPT.md` (the plan, the constraints and the phase checklists) and `docs/RESEARCH.md` before
changing anything. Short version:

- $0 only: no paid Apple account, no DriverKit, no kexts, never ask to disable SIP.
- Apple Silicon, macOS 13+ target, tested on macOS 27 Golden Gate.
- Every hardware constant lives in `Sources/Shared/Mbox3Protocol.h`; update it and its tests when the hardware
  teaches you something, and log the result in `docs/HARDWARE_LOG.md`.
- Original MIT code. Read GhostBox Revenant (GPL-3) and Linux (GPL-2) for facts only; never copy their code.
- `./build.sh test` before every commit; CI must stay green.
- Stop at each hardware checkpoint and ask the user to run it. Remind them of the recovery command:
  `sudo rm -rf /Library/Audio/Plug-Ins/HAL/Mbox3Driver.driver && sudo killall coreaudiod`
