# lib64 design

How the emulator is put together, why, and what each part leaves out. For
building and using it, see the [README](../README.md).

## Shape

Three parts, with a hard boundary around the library:

| | |
|---|---|
| `src/` | The emulator. Platform independent: no Windows headers, no file system, no threads. Builds the `lib64` static library with MSVC or GCC. |
| `app/` | A Windows host built on libdesktop. Owns media files, the C64 input map, menus, the pixel surface, settings and the frame/audio loop. |
| `test/` | A console harness that drives the library headlessly, and its fixtures. |

The library takes bytes and returns bytes. The host reads files, the library
parses buffers; the library fills a frame buffer and a PCM buffer, the host
presents them. Nothing in `src/` knows what a file or a window is.

`machine` is the embeddable façade; `machine_state` is the whole machine as
plain data. `machine` owns exactly one `machine_state` and is non-copyable.
`machine.h` declares everything in the global namespace, and `machine::_state`
is public.

## Execution model

Everything is driven from `cpu_exec(machine_state*, cycles)`. One pass of its
loop is one instruction, and the chips are advanced by exactly the cycles that
instruction took:

```
while (budget remains)
    service NMI / IRQ           (7 cycles, at the instruction boundary)
    KERNAL LOAD trap            (see "Disk" below)
    fetch opcode, execute
    clock_ticks += ticktable[opcode]  (+1 if a page cross earns a penalty)
    step_hardware(cycles just spent) -> tick CIA1, CIA2, VIC
```

There is no scheduler and no event queue. The cost is that a chip can only
observe time at instruction granularity; the benefit is that the whole machine
is one function call with no allocation, which is what keeps it embeddable.

`clock_ticks` is the single clock. Audio, raster position and CIA timers are all
expressed against it, so they cannot drift apart.

A PAL frame is 63 cycles × 312 raster lines = 19656 cycles
(`pal_cycles_per_frame`). Hosts call `exec(pal_cycles_per_frame)` once per
frame.

## Memory bus and banking

`ram_read` / `ram_write` are the only way into memory, and both are a table
lookup on the high byte of the address:

- `read_map[256]` — `0` RAM, `1` BASIC, `2` KERNAL, `3` character ROM, `4` I/O,
  `5` cartridge ROML, `6` cartridge ROMH.
- `write_map[256]` — `0` RAM, `1` I/O. Writes always reach the RAM under a ROM,
  which is why only the `$D0-$DF` pages are ever `1`.

`update_memory_map()` rebuilds both tables from the processor port at `$00`/`$01`
(LORAM/HIRAM/CHAREN, honouring the data direction register) plus the cartridge's
`/EXROM` and `/GAME` lines. It is called on every write to `$00`/`$01` and on
every `$DE00-$DFFF` access, so it early-outs on a cached `memory_map_key`.

Consequences worth knowing:

- Cartridge ROM is only visible while the processor port is *asking* for ROM, so
  a cartridge can write `$34` to `$01` and reach the RAM underneath itself.
- Ultimax (`/GAME` asserted, `/EXROM` not) is handled as its own case: the
  cartridge owns the map, there is no BASIC, KERNAL or character ROM, and I/O is
  visible regardless of CHAREN.
- The I/O chips keep their registers apart from `RAM[]` - the VIC's 64 in
  `vic.regs`, colour RAM in `color_ram`, each CIA's 16 in `regs` - so with I/O
  banked out a write to `$D000-$DFFF` reaches the RAM beneath, where programs
  keep sprites and bitmaps, and banking I/O back in finds the chips unchanged.
  Colour RAM's high nybble reads as `0`, not as open bus. With no cartridge,
  `$DE00-$DFFF` reads `$FF` and ignores writes.

## CPU

`src/cpu.cpp` is a switch-per-opcode interpreter over `machine_state::cpu`.

- Every opcode and addressing mode, correct zero-page and pointer wrapping, and
  the indirect-`JMP` page-boundary bug. The undocumented opcodes run as a 6510
  runs them, since crunchers, loaders and games use them: the stable ones
  (`SLO`, `RLA`, `SRE`, `RRA`, `SAX`, `LAX`, `DCP`, `ISC`, `ANC`, `ALR`, `ARR`,
  `SBX`, `LAS` and the NOPs) exactly, and the unstable ones (`ANE`, `LXA`, `SHA`,
  `SHX`, `SHY`, `TAS`) with the `$EE` constant and the address behaviour most
  C64s show.
- Cycle counts come from `ticktable[256]`; the page-crossing penalty is applied
  only to the indexed read opcodes that actually earn it, gated by
  `penalty_op()`.
- Read-modify-write instructions perform the dummy write. C64 code depends on
  it: that is what makes `INC $D019` and `DEC $DC0D` acknowledge an interrupt.
- Instruction lengths come from `opcodes[256].length` in `opcodes.cpp`, which is
  the single source of truth shared with the disassembler and the assembler.
  `0` marks a JAM.
- NMI is edge-triggered and always taken; IRQ is level-triggered and gated on
  the interrupt-disable flag. `machine::irq()` gives the host a third IRQ source
  alongside the CIA and VIC, cleared once taken.

Not emulated:

- A JAM leaves the CPU spinning on itself, as a halted 6510 would, until reset.
- Indexed addressing makes none of the 6510's dummy reads, which only an I/O
  register with a read side effect can notice.
- Interrupts are sampled at the instruction boundary rather than in an
  instruction's penultimate cycle, and `CLI`, `SEI` and `PLP` take effect
  without the one-instruction delay. Only cycle-exact raster code notices.

## VIC-II

`vic_render_scanline(s, line)` renders one raster line into
`machine_state::framebuffer`, a 384×272 array of packed `0x00RRGGBB`. Rendering
is opt-in (`set_render_enabled`) so headless use stays cheap.

`tick_vic` advances the raster position a line at a time and renders each line
as it *finishes*, so a raster-interrupt handler has had the whole line to change
registers before they are sampled. It answers the cycles the VIC took the bus
for in the span: 40 on a bad line, taken when the line reaches cycle 15, and,
as each line ends, two for each sprite it shows and three for each of the two
groups (sprites 0-2 and 3-7) the VIC fetches them in. `step_hardware` charges
them to the CPU by running the other chips on through them. A frame has bad
lines only if DEN was set on raster `$30`. All five graphics modes and all eight
sprites (expansion, priority, collisions, and an X position near `$1F8`
showing at the left edge, as the PAL X counter wraps) are covered; sprite-background collision uses a per-scanline
foreground mask. Every fetch goes through the VIC's own view of memory: the
character ROM at `$1000-$1FFF` of banks 0 and 2, and an Ultimax cartridge's
ROMH at `$3000-$3FFF`. The borders - the 38-column and 24-row ones too - cover
the sprites, and an invalid mode draws black while its sprites still show and
collide.

Its registers live in `machine_state::vic.regs`, mirrored through
`$D000-$D3FF`; the raster counter, `$D019` latch and `$D01A` mask live beside
them so `read_vic`/`write_vic` can model the read-to-clear and
write-1-to-acknowledge semantics.

Two conventions are pinned by tests: YSCROLL 3 is the neutral position, which
starts character row 0 on raster 51, and a sprite at Y is drawn from raster
Y + 1.

Not emulated:

- A line is drawn from the registers as they stand when it ends, so a change
  made partway along a line - a split colour or a mid-line mode change - shows
  from the next line.
- Stolen cycles are counted, not placed: the CPU loses as many as it would, but
  a bad line's all at once and a line's sprite fetches at its end.
- A collision bit in `$D019` latches whenever a collision register gains a
  sprite during a line, not only on the first collision after `$D01E` or
  `$D01F` is read. Only a program that acknowledges `$D019` without reading the
  collision register can tell.
- DEN blanks the screen on every line it is clear, not once at raster `$30`: a
  game that clears it to blank the screen while loading blanks at once, and a
  mid-frame DEN effect is not exact. Bad lines do follow the raster-`$30` latch.

## SID

`src/sid.cpp` is a cycle-driven 6581/8580 model. Its waveform generators, with
their combined-waveform tables, and its ADSR envelopes follow reSIDfp. Their
output goes through a two-integrator state-variable filter with each chip's own
cutoff curve - the 6581's steep, offset S-curve, the 8580's straight line - and
resonance, then the chip's output stage, where the 6581's DC offset is what
makes a write to the `$D418` volume sound as a sample, and the C64's own audio
filter, a 16 kHz low-pass and a 16 Hz high-pass. A band-limited, two-stage
resampler brings the chip's 985 kHz output down to the host's rate: 44100 Hz
unless `set_audio_sample_rate` asks for another, from 8000 to 96000 Hz. Peaks
past three quarters of full scale are soft-clipped rather than wrapped. The
chip is a 6581 unless the host or a tune's header asks for an 8580.

Register writes are timestamped with `clock_ticks` while audio is enabled, and
`generate_audio()` replays them at cycle accuracy over the span since the last
call. That is what lets a host run a whole frame and *then* ask for its audio
without losing sub-frame register changes. With audio disabled, writes apply
immediately and no samples are produced.

### The `.sid` player

`load_sid()` parses the PSID/RSID header, copies the tune image into RAM and
hand-assembles a driver that calls the tune's `init` and then its `play` from an
interrupt — a CIA timer or a raster IRQ, whichever the header's speed word asks
for. The driver, both IRQ handlers and the NMI handler all live in `$0334-$03FF`
so the text screen at `$0400` stays free. An NTSC tune clocks the SID at
NTSC's 1022727 Hz, and one timed by the vertical blank is played every
263 × 65 cycles, NTSC's frame, so it keeps its pitch and its tempo on the PAL
machine; `reset()` returns the SID to PAL.

That screen is the player's front end. `sid_player_state` (in `machine.h`) keeps
the tune image, its metadata and the selected sub-tune, and `exec()` repaints the
40×25 display each call: name, author, release, chip, timing, elapsed time, the
sub-tune row, per-voice level meters taken from `sid_chip::envelope_level()`, and
the master volume. It writes screen codes and colour nybbles straight into the
memory the VIC reads, and re-asserts `$D011`/`$D016`/`$D018`/`$D020`/`$D021` and
the CIA2 bank bits so a tune that programs the VIC cannot blank the display. If
the tune image occupies `$0400-$07FF` the screen moves to another free 1 KB
page of VIC bank 0.

Input comes from the emulated keyboard matrix, edge-detected against the previous
frame: `1`-`9` restart the tune on another sub-tune and `X` resets the machine.
Nothing here is host specific — a host only feeds keys and shows the frame
buffer, exactly as for any other program.

### Monitor hooks

`capture_sid()` fills a `sid_snapshot` without side effects: the last value the
CPU wrote to each register, since the chip's registers are write-only; each
voice's live envelope level, which advances only while audio is generated; and
per-register write counts that run for the machine's lifetime, so a host diffs
two snapshots to see what a tune drove in between. `set_sid_voice_muted()`
drops a voice in `sid_filter::route_voices`, after the voice has still been
clocked through `output()`, so the waveform generator's state is unchanged and
unmuting picks it up mid-note. A mute is a listening aid, not chip state: it
survives `reset()` and a change of tune. `parse_sid()` reads a header through
the same `read_sid_image()` as `load_sid()`, so the two accept exactly the same
files.

## CIA

One `cia_chip` struct used twice. CIA1 drives IRQ, CIA2 drives NMI; the only
difference is which line an underflow pulls, decided by the caller. Covers timer
latch and reload, one-shot, the force-load strobe, Timer B counting Timer A
underflows, and ICR latch/mask/read-to-clear.

A timer clocked by φ2 underflows when its counter reaches zero; Timer B
counting Timer A underflows only when its counter passes zero, one count later.
A test pins that cascade period.

CIA1 also carries the keyboard matrix (8 rows × 8 columns, active low, scanned
in both directions) and the two joystick ports. CIA2 port A selects the VIC bank,
through its data direction register so undriven lines float high. RESTORE is
not in the matrix: `machine::press_restore()` pulses the NMI line that CIA2's
interrupt output shares, so it makes no edge while CIA2 holds the line low.

The time-of-day clocks count tenths, seconds, minutes and hours, in BCD with an
AM/PM flag, from the 50 Hz mains: CRA bit 7 set divides it by five, clear by
six, as a CIA expecting 60 Hz does. Reading the hours freezes the read-out until
the tenths are read; writing the hours stops the clock until the tenths are
written; with CRB bit 7 set, writes set the alarm instead, which raises ICR
bit 2 when the time reaches it. A clock is stopped from reset until its tenths
are first written. Timer A counts φ2 only while CRA bit 5 is clear; counting
the CNT pin, it holds.

Not emulated: writing CIA1's interrupt mask also recomputes the IR flag (ICR
bit 7), which a 6526 clears only when the ICR is read; CIA2's mask write leaves
the flag alone. Nothing drives CNT, so a timer counting it never moves.

## Cartridge

`cartridge` parses a `.crt` image into `chip_bank`s and exposes the currently
banked ROML/ROMH. Bank switching is per hardware type, driven from the
`$DE00-$DFFF` I/O window; `machine_state` re-derives the page tables afterwards.
The `/EXROM` and `/GAME` lines it reports are what select 8K, 16K or Ultimax.

The types with a handler are Ocean type 1, Fun Play, Super Games, C64 Game
System, Dinamic, Magic Desk, Final Cartridge III, Action Replay, Atomic Power,
Simons' BASIC, Comal 80, Warpspeed, Ross, Westermann, Zaxxon, RGCD and
EasyFlash, with EasyFlash's 256 bytes of RAM at `$DF00`. Any other type,
including Epyx FastLoad and Final Cartridge I, which `cart_type` names, runs as
a plain cartridge in the mode its header sets. Every CHIP packet is treated as
ROM, whatever its type field says. A chip smaller than its window repeats
through it, which is how a 4 KB Ultimax ROM at `$F000` supplies the reset
vector, and in Ultimax mode the VIC sees ROMH at `$3000-$3FFF` of every bank.

## Disk (device 8)

There is no IEC bus and no 1541. Instead the KERNAL's own LOAD routine is
serviced by the host-side model:

- `machine::insert_disk()` copies a `.d64`/`.d71`/`.d81` image into
  `machine_state::disk`, where it survives reset.
- `cpu_exec` compares the PC against **`$F4A5`** before each fetch. That is the
  default target of the LOAD vector at `$0330`, reached by `JMP ($0330)` from
  `$F49E`, which means `$C3`/`$C4` are already populated and the top of the stack
  is the *caller's* return address — so a simulated `RTS` is correct.
- `machine_state::kernal_load()` honours the documented ABI: secondary address 0
  is a relocating load, success returns carry clear with the end address in
  `X`/`Y` and `$AE`/`$AF`, failure returns carry set with `A` = 4.
- It **declines** anything that is not ours (a verify, a device other than 8, an
  empty filename) so the ROM's own error handling still runs.
- `LOAD"$",8` synthesises a listable BASIC directory at `$0801`, and `LOAD"*"`
  takes the first PRG on the disk.

Trapping at the vector's *target* rather than at `$FFD5` is the point: a program
that installs its own fastloader over `$0330` is deliberately not intercepted,
and gets no disk, because pretending to serve it would be a lie about what is
emulated.

`src/disk.cpp` identifies the format from the file size alone and rejects
anything not exactly sized. Every offset is then derived from a validated
geometry, so a malformed image cannot read outside the caller's buffer. A
directory keeps each name's PETSCII bytes, and a name the C64 sends is matched
against them byte for byte, so a name holding graphics characters still loads;
`read_disk_file` and `read_tape_file` take ASCII instead, folding lower case to
the upper case a C64 types. `read_disk_file_entry` and `read_tape_file_entry`
read one entry by the name it stores, its `*` and `?` taken as themselves
rather than as a pattern. `.t64` tape archives are read the same way, for
their files to be loaded.

### Starting a program as a user would

`autostart_disk` and `autostart_prg` cold-start the machine and drive it as a
user at the keyboard would. A disk is inserted and, once the screen editor
waits for input at `$E5CD`, `LOAD"name",8,1` is typed a line at a time into the
keyboard buffer, then `RUN`, or `SYS` to the load address of a file that loads
above BASIC's start. A `.prg` or a `.p00`'s program is put in memory as that
`LOAD` would put it, once BASIC is ready, then run the same way. A character a
name cannot be typed with between quotes is sent as `?`. The typed `LOAD` still
reads the entry it was typed for: `autostart_disk` keeps the name as the
directory stores it in `machine_state::autostart_entry`, and `kernal_load`
reads that entry by its exact name (`read_disk_file_entry`) when the `LOAD`
names it as typed. Without that, a `?` standing for an untypeable character, or
a `*` or `?` in the stored name itself, would be a pattern a file listed
earlier might match. A name no entry has is taken as a pattern, as `LOAD`
takes one. Typing the editor has not taken within five seconds is dropped, so
a program that has taken over the machine never receives it; a cold start that
has not reached the editor in ten seconds is given up the same way.
`autostarting()` is true until the last line is typed or dropped, so a host can
run those frames as fast as it likes.

Not emulated: SAVE, and a disk file read through `OPEN` and `GET#`; G64 images
and a tape drive; the time a load takes, since the trap charges no cycles,
which a loader with a timing watchdog could notice; and a drive's refusal of
unclosed ("splat") files, which load like any other.

## Introspection

`capture_debug()` fills a `debug_state` with the CPU, banking, VIC, CIA and SID
state plus a disassembly window around the PC. It is strictly side-effect free:
latching registers are read from the emulated chip state, never through the bus,
so a debug overlay can never acknowledge an interrupt out from under a program.

`src/assembler.cpp` is a two-pass 6502 assembler over the same opcode table,
used by tests and available to hosts.

## Host application

`app/app.cpp` is a `ui::shell` application linked to libdesktop's `core`,
`platform`, `render` and `ui` libraries. The shell makes the window, its menu
bar and the element tree below it, and offers keys to the menus' accelerators
before the app; the app makes the content - the C64's picture, and beside it
the debug panel and its splitter - and handles drops and splitter input.

A dedicated `platform::serial_executor` emulates one PAL frame at a time,
publishes a triple-buffered snapshot and hands it to the UI thread as a task.
There the snapshot is copied into the picture a `ui::surface_element` shows,
scaled by whole multiples with point sampling: the texture is updated in place
and only the picture is presented again, nothing painted. The debug panel,
drawn with the C64's own character ROM, is a picture of its own beside it. The
host mutex only serializes machine control operations against the worker;
presenting never takes it and never touches the machine.

The app's `platform::audio_stream` is the worker's real-time clock when a device
is available: the worker does not execute another PAL frame until the stream
accepts the current PCM block. The stream's bounded block queue is deliberately
checked before `generate_audio()`, so a full ring cannot advance the SID clock
and discard that frame. Without a device, or once a stream refuses a block after
the capacity check, the worker paces itself at 50 Hz by the wall clock and the
title bar says "(no audio)"; a refused block ends audio for the rest of the
session. A fixed step on the UI thread could neither follow the device's clock
nor run on through a menu or a dialog, so the app keeps its own loop rather
than the shell's: it only waits for events and for the frames the worker hands
over. The offscreen `-test` path uses a small fake `platform::audio_stream` to
pin both refusal paths without opening an audio device.

Key presses and releases reach the machine through `ui::app::key_down`,
`ui::app::key_up` and `machine::queue_key`, and each `exec()` applies the queued
changes at most one per key, so every press and release lasts a whole frame. A
frame is emulated in a burst of about a millisecond; writing the matrix directly
would lose any tap that falls between two bursts — every key the On-Screen
Keyboard or a macro tool types. Joystick presses are latched the same way until
the next frame reads them. Losing the keyboard, to another window or to an open
menu, releases matrix and joystick alike. While paused, nothing runs a frame, so
keys go straight to the matrix. RESTORE is a separate NMI pulse.

## Rules that keep this working

1. `src/` stays platform independent: no `windows.h`, no file access, no
   libdesktop types and no `wchar_t` in new API.
2. `ram_read`/`ram_write` are the only path to memory. Anything that bypasses
   them bypasses banking and I/O.
3. Chips are advanced only from `step_hardware`, and only by cycles actually
   spent. Never advance a chip out of band.
4. One source of truth per fact: `clock_ticks` for time, `opcodes[].length` for
   instruction length, the page tables for banking.
