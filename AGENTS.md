# Agent and contributor guide

Säädin is a fork of Faikout (by RevK / Andrews & Arnold). Firmware only.

## Engineering constraints

- Säädin diverges from Faikout for good. Nothing is sent upstream, and the fork is never rebased onto or merged with upstream. Refactor, rename, or remove upstream code when that serves Säädin.
- Exception: keep the S21 protocol code (`ESP/main/daikin_s21.h`) exactly as upstream has it. Build Säädin features on top of it, not into it.
- Make the smallest change that solves the problem. Match the surrounding style (indentation, naming, comment density) instead of imposing a new one.
- Do NOT enable hardware secure boot, flash encryption, or eFuse burning. These are permanent and can brick the board.
- Use ASCII `saadin` / `Saadin` in code, hostnames, binaries, and topics. Use "Säädin" only in display text.
- Settings are generated, not hand-written. `revk_settings` builds `ESP/main/settings.h` and `settings.c` from `ESP/main/settings.def` (and the library's `settings.def`). Those generated files are git-ignored. Add or change settings in `settings.def`; new demand-control settings (`demand.set`, `pid.kp`, …) go there. Do not edit the generated `settings.h` / `settings.c`.
- Do not commit binaries or build artifacts. Releases are published as GitHub Releases, not stored in the repo.
- Match the existing C style by hand. There is no formatter config; the code uses RevK's GNU-indent style (3-space indent, braces as in `ESP/components/ESP32-RevK/revk.c`). Never reformat lines you did not change, so diffs stay reviewable.

## Git workflow

- Work on a branch and open a PR. Do not commit directly to `main`.
- Never merge PR's without explicit human approval.
- Keep the "Changes from Faikout" section in `README.md` current. Every user-visible change is logged there.

## Building and verifying

- A C change under `ESP/` must build clean with `make <board>` (for example `make s3`, `make pico`) before it is called done.
- Docs, tooling, and repo housekeeping changes need no firmware build.
- Agents do not flash hardware. Flashing over USB and checking behaviour on the real unit is done by the human. State plainly what was built and tested and what was not.

## Testing

- Write a failing (RED) test first, then make it pass. The test must fail for the right reason before the change exists.
- Test how the system behaves or should behave. Do not write tests whose only purpose is to assert that a feature was removed.
- Run logic on the host where possible instead of on hardware:
  - Use the air-con simulators in `Tools/Simulators` (`faikin-s21`, `faikin-x50`, `s21-control`) to exercise protocol and control behaviour without a board. `make` there builds them; they need `popt`.
  - Where it makes sense, factor control logic (for example the demand PI loop and defrost inference) so it compiles and runs as a plain host unit test, separate from the on-device build.
