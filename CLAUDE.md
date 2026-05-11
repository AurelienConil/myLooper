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
| 0 | `STATE_IDLE` | Silent, buffer preserved |
| 1 | `STATE_RECORDING` | Writing input to buffer, no audio output |
| 2 | `STATE_PLAYING` | Continuous looping playback |
| 3 | `STATE_OVERDUBBING` | Playback + mix new input into buffer |
| 4 | `STATE_PAUSED` | Silent, position frozen |
| 5 | `STATE_WAIT_RECORDING` | Armed: plays existing loop, waits for sync BPM to start new recording |
| 6 | `STATE_WAIT_PLAYING` | Armed: silent, waits for sync BPM to start playing |
| 7 | `STATE_PLAYING_ONCE` | Slave: plays one full pass then goes to IDLE (re-triggered at each BPM tick) |
| 8 | `STATE_WAIT_OVERDUBBING` | Slave: plays loop, waits for next BPM tick to start overdubbing |

### Thread-safety pattern

Message handlers (main thread) only write `x->pending_request`. At the **start of each DSP block**, `_perform()` reads and clears it, applies the state transition, then processes audio. Only one request can be pending at a time — a new write overwrites the previous one silently.

Outlet calls from DSP are unsafe (risk of re-entrancy). All outlet notifications go through a `t_clock` at delay 0: the DSP thread writes to `x_notify_state`, `x_notify_duration`, `x_notify_bpm` (sentinel `-1` = no update), `x_notify_master`, then calls `clock_delay(x->x_clock, 0)`. The clock fires in the message thread and sends the actual outlets.

### Buffer management

- Single mono `t_sample *buffer`, 30-second max, allocated at construction via `calloc`.
- Never reallocated during DSP — only at init or on `clear`.
- Loop finalization is triggered by `REQUEST_STOP_RECORDING`, `REQUEST_PLAY` (from `STATE_RECORDING`), `REQUEST_OVERDUB` (from `STATE_RECORDING`), and `REQUEST_WAIT_PLAY` (from `STATE_RECORDING`). Finalization: `finalize_loop_length()` applies zero-crossing trim (backward search, 2048-sample window) then rounds up to the next block boundary. `apply_loop_fades()` then bakes short fade-in/fade-out (`CROSSFADE_LEN = 256` samples ≈ 5ms) at the loop endpoints to suppress wrap clicks.

### Overdub formula

```c
buffer[play_pos] = (buffer[play_pos] * feedback) + (input * overdub_level);
```

Defaults: `feedback = 0.95`, `overdub_level = 0.8`.

### Sync system

- **Master** (default, `x->master = 1`): on every loop wrap during PLAYING or OVERDUBBING, computes `bpm = 60 * sample_rate / loop_length` and sends it via `x_out_sync` (the 3rd outlet, float).
- **Slave** (`x->master = 0`): set automatically when the **tempo inlet** receives a float. On each BPM tick: `STATE_WAIT_RECORDING` → recording; `STATE_WAIT_OVERDUBBING` → overdub (play_pos reset to 0); `STATE_WAIT_PLAYING` or `STATE_IDLE` (after play-once) → `STATE_PLAYING_ONCE`.
- **Reset**: `clear` always sets the instance back to master mode.

### Inlets / Outlets

**Inlets:**
1. Signal + control messages (`CLASS_MAINSIGNALIN`)
2. Secondary control messages inlet (same method table as inlet 1)
3. Tempo float (BPM from a master looper) — routed to `tempo_in` method

**Outlets (left to right):**
1. `x_out_signal` — processed audio signal
2. `x_out_state` — float: current state enum value, sent on every state transition
3. `x_out_sync` — float: BPM sent at each loop wrap when master
4. `x_out_duration` — float: loop duration in milliseconds
5. `x_out_master` — float: 1 = master, 0 = slave, sent on mode change

### Control messages

| Message | Action |
|---|---|
| `record` | **Master**: clear buffer, start recording. **Slave**: arm `STATE_WAIT_RECORDING`, waits for BPM tick to start |
| `stop_recording` | Finalize loop. **Master** → `STATE_IDLE`. **Slave** → `STATE_WAIT_PLAYING` |
| `play` | **Master**: from RECORDING → finalize + `STATE_PLAYING`; from OVERDUBBING → `STATE_PLAYING` (keep position); otherwise → `STATE_PLAYING` (reset position). **Slave**: same transitions but targets `STATE_WAIT_PLAYING` instead of `STATE_PLAYING` |
| `overdub` | **Master**: finalize if from RECORDING, then `STATE_OVERDUBBING`. **Slave from RECORDING**: finalize + `STATE_WAIT_OVERDUBBING` (waits for BPM tick). **Slave otherwise**: `STATE_OVERDUBBING` immediately (already in sync) |
| `stop` | Go to `STATE_IDLE` |
| `pause` | Freeze position, silence output (`STATE_PAUSED`) |
| `clear` | Clear buffer, reset to `STATE_IDLE` + master mode |
| `wait_record` | Arm `STATE_WAIT_RECORDING` (slave, waits for BPM tick) |
| `wait_play` | Arm `STATE_WAIT_PLAYING`; finalizes loop if currently recording |
| `overdub_level <float>` | Set overdub input mix level (0–1, default 0.8) |
| `feedback <float>` | Set overdub feedback level (0–1, default 0.95) |
| `tempo_in <float>` | Direct message equivalent of the tempo inlet |

### Typical workflows

**Simple loop (master):**
```
record → play          (finalize + loop)
```

**Pro layering (master):**
```
record → overdub       (finalize + overdub immediately)
       → play          (clean loop)
       → overdub       (layer again, seamless)
```

**Synced loop (slave, looper-B slaved to looper-A):**
```
record → play          (finalize + waits for next BPM tick from A to start)
```

### Multi-instance sync pattern

```
[looper-A~]  ← master: plays loop, sends BPM on outlet 3
    |
    | (BPM float)
    |
[looper-B~ tempo-inlet]  ← slave: send "record" to arm, then "play" to finalize and wait for sync
```

On `clear` to any instance: that instance reverts to master. A patch typically routes the master's sync outlet to all slave tempo inlets.
