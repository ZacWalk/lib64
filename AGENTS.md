# lib64 Repository Guidance

lib64 is a C++20 Commodore 64 emulator: a platform-independent static library
(`src/`), a Windows host app (`app/`) and a command-line test suite (`test/`).
[README.md](README.md) covers building, running and embedding it, and
[docs/design.md](docs/design.md) how it works and what it does not emulate.
Read the design before changing anything in `src/`.

## Project Boundaries

- `src/` keeps the [rules in the design](docs/design.md#rules-that-keep-this-working)
  and must link into Diffractor and other hosts unchanged.
- `app/` owns media files, the host-key to C64-matrix mapping, presentation,
  menus, settings and the audio clock, all through libdesktop. It includes no
  Windows SDK header and has no Win32 GUI of its own.
- When the app needs a new capability, expose a platform-neutral operation from
  `machine.h` rather than reaching into `machine_state`.

## Coding Guidelines

- C++20 with the existing naming (`snake_case` types and functions).
- `.clang-format` is the layout: tabs, Allman braces, 120 columns. Visual
  Studio installs clang-format in `VC\Tools\Llvm\x64\bin`; `.\dd.ps1 fmt` finds
  it only on PATH and formats `include`, `src` and `tests`, which here is `src`
  alone, so run `clang-format -i` on `app/app.cpp` and `test/emulator-tests.cpp`
  too. `.clang-format-ignore` keeps it off the ROM images, and a table laid out
  by hand is fenced with `// clang-format off` and `// clang-format on`.
- Every target builds without warnings at MSVC's `/W4` and GCC's
  `-Wall -Wextra`, and `.clang-tidy` names the bug-finding and performance
  checks the code is kept clean against. A deliberate exception is a
  `// NOLINTNEXTLINE(check): reason` above its line.
- Keep it compact. This is meant to be a small, fast emulator — add an
  abstraction only when it improves the library API or removes real duplication.
- `src/rom-*.cpp` are ROM images as byte arrays. Never hand-edit them.
- Never edit build output under `bin/` or `build/`.
- Never edit the vendored [dd](https://github.com/ZacWalk/dd) driver, `dd.ps1`
  and `.dd/`; [docs/dd-upstream.json](docs/dd-upstream.json) records their
  hashes. Project settings belong in `dd.psd1`. dd documents its
  [commands](https://github.com/ZacWalk/dd/blob/main/docs/cli.md#commands) and
  its [CLI and MCP modes](https://github.com/ZacWalk/dd/blob/main/docs/mcp.md#two-modes).
- The C runtime is the consumer's choice: `CMakeLists.txt` never sets
  `CMAKE_MSVC_RUNTIME_LIBRARY`, so a standalone build gets CMake's default DLL
  runtime and a host such as Diffractor builds the library with its own.

## Validation

- `.\dd.ps1 test --label lib64` builds both configurations and runs the
  suites. A green build proves nothing on its own: check for `FAIL` lines.
- The library and the suite ship wherever Diffractor does, so a change to
  `src/`, `test/`, the build or a public header must also build without
  warnings and pass in both configurations on Linux (GCC, `-Wall -Wextra`) and
  in Release on 32-bit Windows. [README.md](README.md#build-and-run) has the
  commands. dd's `--json` result counts warnings under `diagnostics`; the x86
  build prints them inline.
- CI ([.github/workflows/build.yml](.github/workflows/build.yml)) builds and
  tests Debug and Release on Windows x64 and on Linux, and Release on Windows
  x86, on every push and pull request.
- Verify `app/` changes with `.\dd.ps1 test --label headless`, which includes
  the app's offscreen `-test` path and console check counts. Launching the app
  is an explicit interactive check, never unattended verification.

## What The Tests Pin

`test/emulator-tests.cpp` is the contract. Before "fixing" an emulation detail,
check whether a test already encodes the current behaviour:

- CPU: results and flags across loads, binary and decimal arithmetic, logic,
  shifts, addressing modes, the stack and branches; cycle counts with the
  page-crossing penalty; the undocumented opcodes' results and flags, and their
  advancing the PC by their table length.
- Memory: the I/O registers and colour RAM apart from the RAM beneath them.
- VIC-II: exact frame-buffer pixels for standard and multicolor text, bitmap
  mode and sprites; collisions and their interrupts; DEN blanking; the
  38-column and 24-row borders, the side border over sprites, an invalid mode's
  black and the character ROM in the VIC's view; the cycles bad lines and
  sprites take; register read-back and the raster interrupt.
- CIA: timer counting, reload, one-shot, force-load, cascade and interrupt
  latching; the time-of-day clock's latch, stop and alarm.
- SID: aliasing, the filter's resonance and both chips' cutoff ranges, output
  level, and an NTSC tune's tempo and pitch.
- Cartridge: a 4K Ultimax ROM's reset vector, and bank switching for EasyFlash,
  Simons' BASIC, Westermann, Zaxxon and RGCD.
- Disk: the KERNAL LOAD contract (carry, `X`/`Y` end address, RAM contents),
  relocating loads, `LOAD"$",8`, the `FILE NOT FOUND` path, PETSCII names,
  `.t64` archives, and starting a disk's file or a `.prg` as a user would.
- Full machine: KERNAL cold boot to `READY.`, `PRINT 6*7` typed through the
  keyboard matrix, key taps shorter than a frame typed through `queue_key`,
  RUN/STOP+RESTORE, SID audio, the `.sid` player and monitor hooks,
  `example.prg`, `1942.d64` and the `.crt` fixtures.

Three conventions are pinned as the code has them: the `$D011` YSCROLL
baseline, the sprite Y offset and the Timer B cascade period, each described in
[docs/design.md](docs/design.md). Changing one means changing its test in the
same commit, deliberately.

No test covers CIA2's interrupts or the other cartridge types' bank switching.
Klaus Dormann's 6502 functional test is not part of the suite; run it by hand
with `--run-bin`.
