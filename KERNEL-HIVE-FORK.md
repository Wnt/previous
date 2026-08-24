# Previous — patch-carrying fork for the Kernel Hive OS museum

Baseline: SVN trunk **r1847** = release **4.4** (`main` is the verbatim import).
Patches live on the **`kernel-hive`** branch, one commit each, so upstream
rebases stay mechanical. Upstream: https://sourceforge.net/projects/previous/
(GPL-2.0).

This fork drives the museum's `nextstep` station **host-native**: no X server,
no window and no host audio device. Previous runs under SDL's dummy video and
audio drivers and carries its own frame, input and sound planes, which the
streamhost daemon consumes directly.

| patch | why |
|---|---|
| `sdlscreen: no imaginary WM borders` | a WM-less session scaled 1120x832 by 0.971, blurring the display and breaking the 1:1 absolute-pointer map |
| `sdlscreen: IFB1 shm framebuffer export` | headless capture: each changed frame published into a shared mapping in the museum's IFB1 wire format |
| `ctlsock: mamectl/1 input control socket` | keyboard and absolute pointer injected over the wire streamhost's existing `mamesock` backend already speaks |
| `sndfifo: raw PCM output on a named pipe` | headless audio: 48 kHz s16le stereo on a non-blocking FIFO, the daemon's `SH_AUDIO_SOURCE=fifo` contract |

Everything is env-gated and inert when the env is unset, so the same binary
still runs as a normal desktop Previous.

## The three planes, and the env that turns them on

| env | plane | streamhost side |
|---|---|---|
| `PREVIOUS_SHM_PATH=<file>` | IFB1 framebuffer mapping | `SH_CAPTURE=shm`, `SH_SHM_PATH=<same>`, **`SH_SHM_DAMAGE=0`** |
| `PREVIOUS_SHM_STATS=1` | publish counters on stderr | — |
| `PREVIOUS_SHM_FULL=1` | publish every repaint whole (control run) | — |
| `PREVIOUS_CTL_SOCK=<path>` | mamectl/1 control socket | `SH_INPUT_BACKEND=mamesock`, `SH_MAMECTL_SOCK=<same>`, `SH_MAMESOCK_KEYMAP=<keymap>` |
| `PREVIOUS_CTL_PTR=auto\|tablet\|kms` | pointer route (default `auto`) | — |
| `PREVIOUS_CTL_PTR_STEP` / `_RATE` / `_SETTLE` | kms-route pacing (see below) | — |
| `PREVIOUS_CTL_BTN_HOLD` | minimum button-down time, ms (default 400) | — |
| `PREVIOUS_AUDIO_FIFO=<path>` | raw PCM output | `SH_AUDIO_SOURCE=fifo`, `SH_AUDIO_FIFO=<same>` |

`SH_MAMESOCK_PTR_GRID` stays **unset**: this server states targets in screen
pixels, which is what the grid-less path already sends.

### Frames

Each repaint diffs the blitted frame against a private shadow; only the changed
region is copied into the mapping, and the header carries a real dirty rect. An
unchanged repaint never touches the seqlock, so the reader never wakes for it.
Measured on an idle NeXTSTEP 3.3 Workspace: **10,200 repaints, 646 published,
27 MB copied, 1.0 ms per repaint** — where publishing every repaint would have
moved 38 GB and made the reader pay for all of it. Because the rect is real,
streamhost should run `SH_SHM_DAMAGE=0` rather than re-deriving one host-side.

`PREVIOUS_SHM_FULL=1` turns the diff off and publishes every repaint whole. It
is the control run for "is this artefact the guest's or mine?", and it earned
its place: diffing the guest's 2 bpp video memory instead of the blitted frame
is 15x faster (59 µs) and **wrong**. The blit reads NEXTVideo on the rendering
thread while the 68k keeps writing it, so a row that changes in between is
shadowed with its new source bytes while the mapping gets the old pixels — and
every later repaint then calls that row unchanged, so the stale pixels stay
forever. It shows up as permanent bands of smeared window texture that vanish
under `PREVIOUS_SHM_FULL=1` on the identical scene. The diff has to run on the
bytes being published.

### Input

The socket speaks **mamectl/1 verbatim** — the same line protocol streamhost's
`mamesock` sink already drives MAME's ctlsock module with — so integration is
env only and needs no new streamhost backend. Verbs: `MOVEA x y`, `MOVEP dx dy`,
`DOWN1..3`/`UP1..3`, `KEY <0|1> <port> <field>`, `PING`; replies `<seq> OK` /
`<seq> ERR <reason>`, and a verb is acked by the EMULATION thread after it is
applied, so a wedged 68k stops acking and the daemon's ack deadline sees it.

Keys use the NeXT KMS space, because that is what the hardware is — a serial
keyboard whose modifiers are a MASK carried with every edge, not keycodes. The
station's `SH_MAMESOCK_KEYMAP` maps browser XT set 1 scancodes onto:

* port **`kms`**, field = NeXT scancode in hex (`0x39` = `a`; see `src/includes/kms.h`)
* port **`mod`**, field = `meta lshift rshift lctrl rctrl lalt ralt`, plus the
  aliases printed on a NeXT keyboard: `control`=meta, `l/rcommand`=l/rctrl,
  `l/ralternate`=l/ralt

An unrecognised port or field is answered `ERR` and dropped, never guessed.

**The pointer has two routes and the choice matters.** With the guest's tablet
driver active (`/NextAdmin/InstallTablet` binds SummaSketch I to serial port B,
which is where `tablet.c` sits) a `MOVEA` is one call and the cursor lands on
the commanded pixel, at any speed. Without it the target is dead reckoned
through the relative NeXT mouse, and NeXTSTEP's own pointer acceleration then
decides what the guest does — measured, 480 counts commanded per run:

| report gap | counts/report | px moved per count |
|---|---|---|
| 20 ms | 1 | **1.00** |
| 20 ms | 2 | 1.20 |
| 20 ms | 4+ | >2.3 (ran into the screen edge) |
| 5 ms | 1 | 1.17 |
| 5 ms | 2+ | >2.3 |

So the kms route is exact only at `PREVIOUS_CTL_PTR_STEP=1
PREVIOUS_CTL_PTR_RATE=20`, which is ~50 px/s. That is fine for bring-up and
useless for a visitor. It is the same shape as the IRIX station's `xset m 1/1 0`
lesson: the fix belongs in the GUEST, and here the guest-side fix is the tablet.
(The same 1.00 gain at one pixel per event, and the same ~2.3x above it, was
measured independently on the live bridged station.)

**Buttons carry a minimum hold.** A ~12 ms press/release on the tablet path is
sampled away entirely — a menu item highlights and never fires, a title-bar drag
does not move the window — while an explicit 400 ms hold works every time. A
browser forwards a visitor's real edges and a quick click is a quick click, so
the floor lives here at the injector, the way `MAME_CTL_KEY_EXCL` serialises
keys for the matrix guests: an early RELEASE waits in the queue, in order, until
`PREVIOUS_CTL_BTN_HOLD` ms have passed since its press.

### Audio

`snd.c` hands over signed 16-bit **big** endian stereo at 44100 Hz (the 22.05 kHz
double modes are already expanded before this point). The FIFO plane byte-swaps
and resamples to the daemon's fixed 48000 Hz s16le stereo contract, on a phase
accumulator carried across blocks so block boundaries add no discontinuity.

The pipe is opened `O_RDWR|O_NONBLOCK` and **drops what does not fit**, padding
back to the 4-byte frame boundary. This is not a nicety: nothing drains a
station's audio pipe until a visitor connects, and a blocking write parks the
emulator inside `write()` servicing no control socket, no checkpoint restore and
no frames — the failure the VICE conversion hit on a live station.

## Checkpointing (CRIU)

Dump and restore work with all three planes open. Measured on the bring-up rig:
**dump 0.15 s, restore 0.13 s**, flags `--shell-job --file-locks
--manage-cgroups=ignore`, with the process SIGSTOPped and the disk image
reflink-paired before the dump. After restore the process is still in its
job-control stop: **`kill -CONT` it**.

One constraint the control socket adds: a dump succeeds with the socket
LISTENING but fails while a client is CONNECTED (`unix: Unix socket … not
found`), and `--ext-unix-sk` does not rescue it. Take checkpoints with no
client attached; the daemon's `mamesock` sink already reconnects forever with
backoff, so a restore simply gets reconnected.
