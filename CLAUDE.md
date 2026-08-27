# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```bash
# Build (requires Pd headers — auto-detected if Pd is in /Applications)
make

# Build with explicit Pd include path
make pdincludepath=~/pd/src/

make clean
```

The build system is [pd-lib-builder](https://github.com/pure-data/pd-lib-builder), included as a git submodule in `pd-lib-builder/`. Output binary: `myLooper~.pd_darwin` (macOS).

Only `myLooper~.c` is compiled (`class.sources` in the Makefile).

## Architecture

`myLooper~` is a Pure Data DSP external (tilde object) implementing an audio looper. Everything lives in `myLooper~.c`.

### State machine

9 states (`t_loop_state`):

| Value | Name | Behaviour |
|-------|------|-----------|
| 0 | `STATE_IDLE` | Silent, no loop recorded (`loop_length == 0`) |
| 1 | `STATE_RECORDING` | Writing input to buffer, no audio output |
| 2 | `STATE_PLAYING` | Continuous looping playback |
| 3 | `STATE_OVERDUBBING` | Playback + mix new input into buffer |
| 4 | `STATE_STOP` | Silent, position frozen, but a loop was recorded and then explicitly stopped (`end_play`, `loop_length > 0`) |
| 5 | `STATE_WAIT_RECORDING` | Armed: plays existing loop, waits for sync BPM to start new recording |
| 6 | `STATE_WAIT_PLAYING` | Armed: silent, waits for sync BPM to start playing |
| 7 | `STATE_PLAYING_ONCE` | Plays one full pass then goes to `STATE_WAIT_PLAYING` (re-triggered at each tempo tick). Used by slaves, and by the master right after it finalizes its first recording — nothing free-runs on its own tempo once a shared tempo exists |
| 8 | `STATE_WAIT_ENDREC` | Slave: `end_record` was called mid-`RECORDING`; still recording (identical audio behaviour to `STATE_RECORDING`), waiting for the next BPM tick to finalize |

Recording is only ever ended by `end_record` — `play`, `overdub`, and `wait_play` all reject (`pd_error`) while `STATE_RECORDING` or `STATE_WAIT_ENDREC`, master or slave alike. There is no "record → overdub" or "record → play" shortcut; those messages require an already-finalized loop.

`STATE_IDLE` and `STATE_STOP` are both silent, but distinguish whether a loop exists: `STATE_IDLE` means `loop_length == 0` (fresh instance, or right after `clear`); `STATE_STOP` means a loop was recorded and is now stopped. `end_play` picks one or the other based on `loop_length > 0` at the moment it fires, so patches can tell from the state outlet alone whether there's anything to `play`/`overdub`. `end_record` (master) never lands on `STATE_STOP` — it goes straight to `STATE_PLAYING_ONCE` when a loop was captured (this is also the moment the shared tempo is born, see Sync system below), or `STATE_IDLE` otherwise.

### Thread-safety pattern

Message handlers (main thread) only write `x->pending_request`. At the **start of each DSP block**, `_perform()` reads and clears it, applies the state transition, then processes audio. Only one request can be pending at a time — a new write overwrites the previous one silently.

A second field, `x->armed_finalize` (a plain 0/1 flag), handles the slave-specific case of ending a recording: `end_record` called while a **slave** is in `STATE_RECORDING` doesn't write a finalizing `pending_request` directly (which would cut the recording immediately, at an arbitrary point relative to the master's tempo). Instead it sets `armed_finalize = 1` and requests `REQUEST_ARM_WAIT_ENDREC`, which just switches the reported state to `STATE_WAIT_ENDREC` — recording keeps running underneath, identically to `STATE_RECORDING`. `tempo_tick()` checks `armed_finalize` on every tick from the shared metronome; if set (state is `STATE_RECORDING` or `STATE_WAIT_ENDREC`), it requests `REQUEST_TICK_FINALIZE_PLAY` (fired at the start of the next DSP block, same as any other tempo-triggered transition) and clears the flag. This guarantees the finalized loop length is an exact multiple of the master's period. `armed_finalize` is also cleared on `REQUEST_RECORD`, `REQUEST_STOP`, and `REQUEST_CLEAR` to avoid a stale arm firing on a later, unrelated recording.

Outlet calls from DSP are unsafe (risk of re-entrancy). All outlet notifications go through a `t_clock` at delay 0: the DSP thread writes to `x_notify_state`, `x_notify_duration`, `x_notify_sync_length`/`x_notify_sync_bpm` (sentinel `-1` on `x_notify_sync_length` = no update), `x_notify_master`, `x_notify_isplaying`, `x_notify_isloop`, then calls `clock_delay(x->x_clock, 0)`. The clock fires in the message thread and sends the actual outlets.

`x_notify_isplaying`/`x_notify_isloop` differ from the others in one respect: `loop_length` can grow sample-by-sample while `STATE_RECORDING`/`STATE_WAIT_ENDREC` is running, with no `pending_request` involved. So rather than being set only inside the request-handling switch, they're derived and diffed against `x->x_prev_isplaying`/`x->x_prev_isloop` unconditionally at the end of every DSP block in `_perform()`, and only queued for output when they actually flip.

### Buffer management

- Single mono `t_sample *buffer`, 30-second max, allocated at construction via `calloc`.
- Never reallocated during DSP — only at init or on `clear`.
- Loop finalization is triggered by `REQUEST_STOP_RECORDING` (master `end_record`) and `REQUEST_TICK_FINALIZE_PLAY` (slave, fired by `armed_finalize` on a BPM tick after `end_record`). Finalization: `finalize_loop_length()` applies zero-crossing trim (backward search, 2048-sample window) then rounds up to the next block boundary. `apply_loop_fades()` then bakes short fade-in/fade-out (`CROSSFADE_LEN = 256` samples ≈ 5ms) at the loop endpoints to suppress wrap clicks.

### Overdub formula

```c
buffer[play_pos] = (buffer[play_pos] * feedback) + (input * overdub_level);
```

Defaults: `feedback = 0.95`, `overdub_level = 0.8`.

### Sync system

The tempo clock is external to any looper instance — a shared metronome object, built at the patch level (not part of this external), that the master triggers once and that then ticks independently of any looper's play state. This is deliberate: if the master's own playback were the tick source (as in an earlier design), pausing the master would silence sync for every other instance. Decoupling the two means no instance is ever load-bearing for the others' sync.

- **Master** (default, `x->master = 1`): the instance that records the first, unconstrained loop. On `end_record` (or on filling the buffer), once finalized, it sends **one** list message `(loop_length bpm)` via `x_out_sync` (the 3rd outlet) — `loop_length` in raw samples is what actually seeds the shared metronome's period (fed via a patch-level `[metro]` set to sample units); `bpm` is purely informational, for display, computed once via `compute_loop_bpm()`. A loop is assumed to span 1, 4, 8, or 16 beats (never a single arbitrary duration), and tempos are assumed to fall in 80–160 BPM; since those four beat counts produce disjoint BPM ranges for any given loop duration, the beat count — and thus the true BPM — is uniquely recoverable from `loop_length` alone (`bpm = 60 * beats * sample_rate / loop_length`, picking the `beats` in {1,4,8,16} that lands the result in [80,160]). After sending this message, the master itself plays that first pass once (`STATE_PLAYING_ONCE`) and then falls into the same tick-driven regime as every other instance — it does not free-run.
- **Slave** (`x->master = 0`): set automatically when the **tempo inlet** receives a float — including the master itself, once its own sync message loops back through the shared metronome into its tempo inlet. On each tick: `STATE_RECORDING`/`STATE_WAIT_ENDREC` with `armed_finalize` set → finalize (`REQUEST_TICK_FINALIZE_PLAY`) straight to `STATE_PLAYING_ONCE`; `STATE_WAIT_RECORDING` → recording; `STATE_WAIT_PLAYING` (including a just-finished play-once pass) → `STATE_PLAYING_ONCE`. A tick that arrives while an instance is mid-pass (`STATE_PLAYING_ONCE`) is a no-op — ticks are only consumed in `STATE_WAIT_PLAYING` — so an instance always re-locks on the first tick available after its own pass ends, regardless of how many beats its own loop spans relative to the metronome's tick rate. This is also what keeps `loop_length`'s rounding error (see Buffer management) from accumulating: every pass is corrected against the tick, never left to drift freely.
- **Reset**: `clear` always sets the instance back to master mode — a new tempo can only be born by recording again from an instance in master mode. `clear slave` instead resets straight into slave mode, for instances that should never seed their own tempo (e.g. one added to a patch after the shared tempo already exists).

### Inlets / Outlets

**Inlets:**
1. Signal + control messages (`CLASS_MAINSIGNALIN`)
2. Secondary control messages inlet (same method table as inlet 1)
3. Tempo tick (bang from the shared metronome) — routed to `tempo_tick`. Filters strictly on `bang`; a stray float here is rejected by Pd, not silently accepted

**Outlets (left to right):**
1. `x_out_signal` — processed audio signal
2. `x_out_state` — float: current state enum value, sent on every state transition
3. `x_out_sync` — list `(loop_length bpm)`: sent once by the master when it finalizes its first recording, to start the shared external metronome (see Sync system)
4. `x_out_duration` — float: loop duration in milliseconds
5. `x_out_master` — float: 1 = master, 0 = slave, sent on mode change
6. `x_out_isplaying` — float: 1 unless state is `STATE_IDLE` or `STATE_STOP`, sent whenever it flips (see Thread-safety pattern)
7. `x_out_isloop` — float: 1 when `loop_length > 0`, sent whenever it flips

### Control messages

Messages are paired start/end: `record`/`end_record`, `overdub`/`end_overdub`, `play`/`end_play`. `clear` stands alone (hard reset).

Recording is a strict two-step operation: `record` then `end_record`. `play`, `overdub`, and `wait_play` all require a finalized loop and reject (`pd_error`) while `STATE_RECORDING` or `STATE_WAIT_ENDREC` — there is no shortcut that ends a recording implicitly.

| Message | Action |
|---|---|
| `record` | **Master**: clear buffer, start recording. **Slave**: arm `STATE_WAIT_RECORDING`, waits for BPM tick to start |
| `end_record` | Requires `STATE_RECORDING` (else no-op). **Master**: finalize immediately → `STATE_PLAYING_ONCE` (reset position) and emit `(loop_length bpm)` on `x_out_sync` to start the shared tempo, or `STATE_IDLE` if nothing was captured. **Slave**: arm `STATE_WAIT_ENDREC` — recording continues until the next tempo tick, which finalizes the (now sync'd) loop and goes straight to `STATE_PLAYING_ONCE` |
| `play` | Rejected (`pd_error`) if `STATE_RECORDING`/`STATE_WAIT_ENDREC`. **Master**: from OVERDUBBING → `STATE_PLAYING` (keep position); otherwise → `STATE_PLAYING` (reset position). **Slave from OVERDUBBING**: immediate `STATE_PLAYING` (keep position, no tick wait) — already sync'd and running continuously, same as master. **Slave otherwise**: arm `STATE_WAIT_PLAYING` |
| `end_play` | Go to `STATE_STOP` if a loop exists, else `STATE_IDLE`; also cancels any armed finalize |
| `overdub` | Requires `STATE_PLAYING`, `STATE_PLAYING_ONCE`, or `STATE_OVERDUBBING` (else `pd_error`) — the loop must actively be playing. `STATE_OVERDUBBING` immediately (position kept), master and slave alike — already in sync |
| `end_overdub` | Requires `STATE_OVERDUBBING` (else `pd_error`). Immediate `STATE_PLAYING` (keep position), master and slave alike — equivalent to `play` from `STATE_OVERDUBBING` but explicit |
| `clear` | Clear buffer, reset to `STATE_IDLE` + master mode; also cancels any armed finalize |
| `clear slave` | Same as `clear`, but resets into slave mode (`x->master = 0`) instead of master — for pre-arming an instance to join an already-running tempo without ever recording the first loop itself |
| `wait_record` | Arm `STATE_WAIT_RECORDING` (slave, waits for BPM tick) |
| `wait_play` | Rejected (`pd_error`) if `STATE_RECORDING`/`STATE_WAIT_ENDREC`. Otherwise arm `STATE_WAIT_PLAYING` |
| `overdub_level <float>` | Set overdub input mix level (0–1, default 0.8) |
| `feedback <float>` | Set overdub feedback level (0–1, default 0.95) |
| `tempo_tick` | Internal target of the tempo inlet's bang; not typically sent directly |
| `tempo_in <float>` | Direct message equivalent of a tempo tick, for manual testing without a real metronome — the float is stored (`x->bpm`) but not otherwise used |

### Typical workflows

**Simple loop (master, no other instance involved):**
```
record → end_record          (finalize, plays once, then re-triggers on its own metronome tick)
```

**Pro layering (master):**
```
record → end_record    (finalize → STATE_PLAYING_ONCE, tempo born)
       → overdub        (start layering)
       → end_overdub    (clean loop)
       → overdub        (layer again, seamless)
```

**Synced loop (slave, looper-B slaved to the shared tempo):**
```
record → end_record          (waits for next tempo tick to finalize + start playing)
```

### Multi-instance sync pattern

The tempo is not owned by any looper — it's a shared metronome object built at the patch level, seeded once and then run independently:

```
[looper-A~]  ← records the first loop; on end_record, sends (loop_length bpm) once on outlet 3
    |
    | (loop_length bpm)
    v
[metronome]  ← patch-level object (e.g. [metro], set to sample units): takes loop_length
    |          as its period, starts ticking on receipt, keeps ticking on its own from then on
    | (bang)
    +---------------------+---------------------+
    v                     v                     v
[looper-A~ tempo-inlet]  [looper-B~ tempo-inlet]  [looper-C~ tempo-inlet]  ...
```

Critically, **looper-A's own tempo inlet is wired back into the metronome's output too** — once its sync message has started the metronome, A re-locks its own playback to that shared tick exactly like every other instance (`tempo_tick()` flips it out of master mode on the first tick it receives). This is what makes pausing (`end_play`) any single instance, including A, harmless to the others: the metronome runs independently of all of them.

The tempo inlet only accepts `bang` (see Inlets above) — it filters strictly, so wiring a float into it by mistake is rejected by Pd rather than silently doing nothing.

On `clear` to any instance: that instance reverts to master mode, ready to seed a new tempo the next time it finalizes a recording. `clear slave` instead pins it to slave mode.
