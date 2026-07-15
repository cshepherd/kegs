# KEGS Debug Socket

KEGS (Apple IIgs emulator) has been patched to expose its built-in debugger
over a localhost TCP socket. `tools/kegs_dbg.py` is a small Python client.

## Setup (one-time, already done)

The patched KEGS lives at `/Applications/Apple IIGS/kegs.1.34/`. The patch
adds:

- A `-dbgport <n>` command-line flag that opens a listening socket on
  `127.0.0.1:<n>` and boots the emulator halted.
- A line-oriented wire protocol on that socket. Every line you send is run
  through KEGS's built-in monitor-style debugger (`do_debug_cmd`). Every
  line the debugger emits — including asynchronous breakpoint-hit
  notifications — is mirrored to the socket.

Posix-only, single-client, localhost-only.

## Running KEGS for a debug session

```bash
cd "/Applications/Apple IIGS/kegs.1.34"
./KEGSMAC.app/Contents/MacOS/KEGSMAC -dbgport 6510 &
```

The emulator window will open and the IIgs will be **halted at boot**.
Nothing runs until you connect and issue `g`.

To use a different port, change `6510` on both sides. 6501 and 6502 are
already used by KEGS for emulated SCC serial ports — pick something else.

## Using the Python client

```python
from kegs_dbg import Kegs

k = Kegs(port=6510)              # connect; raises if KEGS isn't listening

print(k.regs())                  # CPU registers
print(k.read_mem(0xe1, 0, 0x1f)) # hex dump of e1/0000..001f
k.set_bp(0x00, 0xc50a)           # break on ProDOS MLI entry
k.go()                           # resume
print(k.wait_for_halt())         # blocks until a breakpoint fires
print(k.regs())                  # inspect state at the hit
k.step()                         # step one instruction
k.halt()                         # async halt of a running emulator
k.cmd("e1/0010.0020")            # send an arbitrary debugger command

k.close()                        # disconnect
```

### Client API

| Method | What it does |
|---|---|
| `Kegs(host, port, timeout)` | Connect. Defaults: `127.0.0.1`, `6510`, `5.0`s |
| `cmd(line)` | Send any debugger command, return its output text |
| `regs()` | CPU registers (PC, A, X, Y, P, S, D, B, cycle count) |
| `read_mem(bank, start, end)` | Hex dump `bank/start .. bank/end` (inclusive) |
| `set_bp(bank, addr)` | Set execution breakpoint |
| `clear_bp(bank, addr)` | Delete one breakpoint |
| `list_bps()` | Show all breakpoints |
| `go()` | Resume execution |
| `step()` | Single-step one instruction |
| `halt()` | Force the running emulator back into the debugger |
| `reset()` | Reset the IIgs |
| `wait_for_halt(timeout)` | Block until a breakpoint hit message arrives |
| `close()` | Disconnect (sends `bye`) |

### Reading the responses

Every method returns the raw text the debugger emits, with one minor
artifact: when the emulator halts at a breakpoint, the debugger's
on-screen prompt line (`> ` plus a cursor byte) leaks into the stream.
It's harmless — strip it if you need clean text. The sentinel `(kegs)\n`
marks the end of each command's response and is consumed by the client.

## Wire protocol (for writing other clients)

```
client→server:   <command>\n
server→client:   *<echoed-command>\n
                 <output lines>\n
                 (kegs)\n
```

Async events (breakpoint hits, halt notifications) arrive between
prompts without a sentinel. Two socket-level commands are intercepted
before the debugger sees them:

| Command | Effect |
|---|---|
| `halt` | Calls `set_halt(1)` to force a running emulator into the debugger |
| `bye` / `quit` | Close the client connection (does **not** quit KEGS) |

You can talk to it with `nc`:

```bash
nc 127.0.0.1 6510
```

## Apple IIgs addressing reminder

24-bit addresses are written `BB/AAAA` (bank/offset). Useful banks:

| Bank | Contents |
|---|---|
| `00`, `01` | RAM (first 128 KB) |
| `02`–`7f` | Extended RAM |
| `e0`, `e1` | "Bank 0/1" shadowed system RAM (system globals, toolbox vectors, ProDOS) |
| `f0`–`ff` | ROM ($fc–$ff is the actual 256 KB ROM) |

A few well-known entry points:

| Address | What |
|---|---|
| `00/c50a` | ProDOS MLI entry (the smartport firmware calls into here) |
| `e1/0010` | Interrupt jump table entry |
| `e1/0000`..`000f` | RESET/COP/BRK/etc. native vectors |

## Underlying debugger commands

`Kegs.cmd(line)` passes any string to KEGS's debugger verbatim. Cheat sheet:

| Command | Effect |
|---|---|
| `BB/AAAAg` | Go from `BB/AAAA` |
| `g` | Go from current PC |
| `s` | Step one instruction |
| `BB/AAAAB` | Set execution breakpoint |
| `B` | List breakpoints |
| `BB/AAAAD` | Delete one breakpoint |
| `BB/AAAA.AAAA` | View memory (hex dump) |
| `BB/AAAAL` | Disassemble at address |
| `q` or `Q` or `Ctrl-E` | Dump registers |
| `r` | Reset the machine |
| `0=m` / `1=m` | Set m bit (8/16-bit accumulator) for listings |
| `0=x` / `1=x` | Set x bit (8/16-bit index) for listings |
| `<mode>V` | XOR verbose flags (1=DISK, 2=IRQ, 4=CLK, 8=SHADOW, 0x10=IWM, 0x20=DOC, 0x40=ABD, 0x80=SCC, 0x100=TEST, 0x200=VIDEO) |
| `<mode>H` | XOR halt-on flags (1=SCAN_INT, 2=IRQ, 4=SHADOW_REG, 8=C70D_WRITES) |
| `BB/AAAA.AAAAus<file>` | Save memory range to file |
| `BB/AAAA.AAAAul<file>` | Load memory range from file |

Multi-address shorthand: after `e1/0010B`, a bare `14B` sets a second
breakpoint at `e1/0014` (the bank is remembered).

## Common recipes

**Disassemble around the current PC**
```python
pc_line = k.regs()                       # e.g. "PC=00.22b0 ..."
# parse out the PC, then:
print(k.cmd("00/22b0L"))
```

**Watch a memory location for changes**
```python
k.set_bp(0xe1, 0x0010)                   # by default breaks on execute
# For read/write watchpoints, KEGS' built-in `B` is execute-only.
# Use the verbose flag for a coarser trace, or add a more specific
# debug pattern to the C source.
```

**Walk through a routine**
```python
k.set_bp(0x00, 0xc50a)
k.go()
k.wait_for_halt()
for _ in range(20):
    print(k.step())
```

**Dump a sprite buffer**
```python
print(k.read_mem(0x02, 0x2000, 0x21ff))
```

## Test fixtures (input runbooks)

KEGS can record a session's keyboard and mouse input with emulated-cycle
timestamps and play it back deterministically — a Playwright-style test
fixture. At the end of a playback script the emulator **halts into the
debugger**, so a client on the debug socket can inspect the resulting
machine state.

### Command line

```bash
./KEGSMAC.app/Contents/MacOS/KEGSMAC -record steps.kfix          # record from power-on
./KEGSMAC.app/Contents/MacOS/KEGSMAC -playback steps.kfix -dbgport 6510
```

While recording, **F10 stops the recording** (F10 rather than ESC, so ESC
keypresses can themselves be recorded). Stopping writes a final `W` wait
record stamped at the moment F10 was hit, so playback runs on to that same
moment — leave a settle pause before pressing F10 and the fixture will
include it. During playback F10 aborts the script without halting.

For reproducible fixtures, record from power-on with the same disk images
and config; playback re-injects events at the same emulated cycle counts
(to within one 60 Hz frame).

### Debugger / socket commands

| Command | Effect |
|---|---|
| `testfix record FILE` | Arm recording (starts when emulation next runs) |
| `testfix play FILE` | Arm playback; halts into the debugger at script end |
| `testfix stop` | Stop recording (writes the file) or abort playback |
| `testfix` / `testfix status` | Show mode, event count, current cycle |

A typical agent loop over the socket: `testfix play fixture.kfix`, then
`g` if halted, then wait for the async `testfix: playback complete,
halting` message, then inspect registers/memory as usual. Emulated time
freezes while halted, so a breakpoint that fires mid-script simply pauses
the remaining playback until you `g` again.

### Runbook file format

Plain text, one event per line, `#` comments allowed, editable by hand:

```
KEGSFIX1
K <cycle> <raw_a2code hex> <unicode hex> <is_up>     # key transition
M <cycle> <x> <y> <button_states> <buttons_valid>    # mouse move/buttons
W <cycle>                                            # wait until cycle
```

`<cycle>` is a decimal count of emulated cycles (`g_cur_dfcyc >> 16`,
roughly 1.02 MHz) since the recording started. Key events use the raw
Apple ADB keycode plus the unicode character the host driver supplied
(e.g. `K 3000000 00 0061 0` presses `a`, `K 3060000 00 0061 1` releases
it). Mouse `x`/`y` are A2 screen coordinates (0-639, 0-399);
`button_states` bit 0 is the left button and `buttons_valid` masks which
button bits to apply.

## Troubleshooting

- **`ConnectionRefusedError`** — KEGS isn't running or wasn't started
  with `-dbgport`. The flag is positional, not part of `config.kegs`.
- **`busy, debugger already attached`** — only one client at a time.
  Disconnect the other and reconnect.
- **No prompt appears / hang** — the emulator is mid-frame; default
  timeout is 5 s. If you sent `g`, the emulator is running normally
  and won't prompt until a breakpoint fires — use `wait_for_halt()`
  instead of `cmd()`.
- **`bye` doesn't quit KEGS** — it only closes the socket. Use
  `kill <pid>` or close the emulator window to quit KEGS itself.
