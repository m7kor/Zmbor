# Scripted controller playback

Set `ZMBOR_PAD_SCRIPT` to a UTF-8/ASCII text file before starting the runtime.
Playback starts at `scePadInit` (and restarts if the guest initializes pads again).
Without this variable, normal keyboard/controller input is unchanged. The existing
explicit pad override API takes precedence over playback.

The first non-comment line selects the clock:

```text
timeline frames
# tick port slot pressedMask lx ly rx ry
0   0 0 0      128 128 128 128
600 0 0 0x0008 128 128 128 128 # START down
630 0 0 0      128 128 128 128 # release
700 0 0 0x4000 255 128 128 128 # CROSS and left stick right
730 0 0 0      128 128 128 128
```

Use `timeline milliseconds` for a wall-clock schedule. Frame schedules use actual
guest VBlank ticks, relative to pad initialization; they do not count pad polls or
host presentations. A stalled guest does not advance a frame schedule. Exact
game execution determinism still depends on the scheduler and other devices.

Rows contain complete states, held until the next row for that port. Before its
first event a port is neutral. Include explicit release/recentering rows. Both
ports 0 and 1 are supported, slot 0 only. Scripted input owns both ports, preventing
live input from affecting playback. Events are sampled when the guest reads the
pad; a press and release entirely between reads will not be observed by the guest.

Numbers are decimal or prefixed `0x` hexadecimal. The pressed mask is active-high:
`SELECT=0x0001`, `L3=0x0002`, `R3=0x0004`, `START=0x0008`, `UP=0x0010`,
`RIGHT=0x0020`, `DOWN=0x0040`, `LEFT=0x0080`, `L2=0x0100`, `R2=0x0200`,
`L1=0x0400`, `R1=0x0800`, `TRIANGLE=0x1000`, `CIRCLE=0x2000`, `CROSS=0x4000`,
`SQUARE=0x8000`. Combine bits to press multiple buttons. Axes range from 0 to 255,
center 128. Output bytes retain the runtime's existing active-low PS2 encoding.

Ticks must be globally nondecreasing; the last same-tick row for a port wins.
Comments begin with `#`. Up to 100,000 events are accepted. Malformed or unreadable
files produce an explicit stderr error and neutral scripted input; partial
sequences and live input are not used after a script error.

Stderr `[pad:playback]` entries record each newly observed event with its scheduled
tick, actual relative frame and milliseconds, pressed mask and axes. Repeated polls
of the same event do not generate duplicate logs. This records the state selected
at reads, rather than claiming that an event between reads reached the guest.

Example:

```powershell
$env:ZMBOR_PAD_SCRIPT = 'C:\path\to\pad-script.txt'
# Launch the native PS2 program you built with Zmbor (path depends on your build).
& .\build\ps2xRuntime\ps2x_game.exe
Remove-Item Env:ZMBOR_PAD_SCRIPT
```

The example row timing illustrates the format; it does not establish that any
specific game reaches a menu at frame 600. No Crash Twinsanity checkout or
workstation-specific pipeline script is needed for PAD playback.
