# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```bash
# Build (requires Pure Data source headers on path, or specify explicitly)
make

# Build with explicit Pd include path
make pdincludepath=~/pd/src/

# Clean build artifacts
make clean
```

The build system is [pd-lib-builder](https://github.com/pure-data/pd-lib-builder), included as a git submodule in `pd-lib-builder/`. The output binary is `myLooper~.pd_darwin` (macOS), `.pd_linux` (Linux), or `.dll` (Windows).

The `util/duration.c` file exists but is **not compiled** — it is not listed in `class.sources` in the Makefile. Only `myLooper~.c` is compiled.

## Architecture

`myLooper~` is a Pure Data DSP external (tilde object) implementing an audio looper. Everything lives in `myLooper~.c`.

### State machine

The looper has 7 states (`t_loop_state`):

| State | Value | Behaviour |
|-------|-------|-----------|
| `STATE_IDLE` | 0 | Silent, holds recorded buffer |
| `STATE_RECORDING` | 1 | Writing input to buffer, no output |
| `STATE_PLAYING` | 2 | Playback loops continuously |
| `STATE_OVERDUBBING` | 3 | Playback + mix new input into buffer |
| `STATE_PAUSED` | 4 | Silent, position frozen |
| `STATE_WAITING_SYNC` | 5 | Plays existing loop, waits for sync bang to change state |
| `STATE_PLAYING_ONCE` | 6 | Plays buffer once then goes IDLE |

### Thread-safety pattern (critical)

Pd runs message handlers on the main thread and `_perform()` on the audio thread. **Direct state mutation from message handlers causes race conditions.**

The solution used here: message handlers only write `x->pending_request` (a `t_loop_request` enum). At the **start of each DSP block**, `mylooper_tilde_perform()` reads and clears `pending_request`, applies the state transition, then processes audio. This means there is a latency of up to one audio block (~1–10 ms) before a command takes effect, which is acceptable for a looper.

Only one request can be pending at a time — writing a new request before the previous one is processed will silently overwrite it.

### Buffer management

- A single mono `t_sample *buffer` is allocated at construction time via `calloc` (default 30 seconds at 44100 Hz, or current sample rate if already known).
- Buffer is **never reallocated during DSP** — only at init or on `clear`.
- On stop-recording, `loop_length` is rounded **up** to the nearest multiple of the block size `n` to keep loop boundaries block-aligned. Extra samples are zero-filled.
- `write_pos` advances during recording; `play_pos` advances during playback/overdub. Both are plain indices, no interpolation.

### Overdub formula

```c
buffer[play_pos] = (buffer[play_pos] * feedback) + (input * overdub_level);
```

`feedback` (default 0.95) controls how much existing audio decays each pass. `overdub_level` (default 0.8) scales the new input being mixed in.

### Sync system

- A looper can be `master` (sends a `bang` on `x_out_sync` at each loop wrap) or `slave` (receives bangs via the `sync_in` message).
- A slave in `STATE_WAITING_SYNC` transitions to RECORDING or PLAYING when it receives `sync_in`.
- A slave in `STATE_IDLE` with a recorded buffer responds to `sync_in` by scheduling a `REQUEST_PLAY_ONCE` — designed for one-shot sync triggering.
- The `bpm` and `beats_per_loop` fields exist in the struct but are **not used** in the current DSP logic. They are set-able via messages but have no effect.

### Outlets (left to right)

1. `x_out_signal` — processed audio signal
2. `x_out_state` — float: current state enum value (sent on every state transition)
3. `x_out_sync` — bang: sent at loop wrap in master mode
4. `x_out_duration` — float: loop duration in milliseconds (sent on state transitions that finalise or start playing a loop)

### `just_finished_playonce` flag

A protection flag set when `STATE_PLAYING_ONCE` reaches the end of the buffer. It is cleared at the end of the next DSP cycle **only if** the next pending request is not another `REQUEST_PLAY_ONCE`. This prevents a sync-driven rapid re-trigger from reading a stale state.

## Known limitations relevant to multi-loop work

- **Single buffer, single state machine** — the struct holds exactly one loop. A multi-loop design requires either an array of loop structs or a separate class for each slot.
- **Single pending request** — only one command can be queued at a time. A queue (even a small ring buffer) would be needed if commands can arrive faster than one per block.
- **No crossfading** — state transitions (record→play, overdub→play) are instantaneous, which can produce clicks.
- **No interpolation** — playback speed is always 1× (no pitch-shifting or time-stretching).
- **`outlet_*` calls inside `_perform()`** — technically unsafe per Pd internals but widely done; calls like `outlet_float(x->x_out_duration, ...)` happen inside the DSP perform routine for state change notifications.
- **`util/duration.c` is unused** — the conversion helpers it defines (`samples_to_ms`, `ms_to_samples`, `beats_to_ms`, `ms_to_beats`) are duplicated inline in `myLooper~.c` and the file is never compiled.
