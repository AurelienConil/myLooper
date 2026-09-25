#include "m_pd.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define CROSSFADE_LEN 256  /* ~5ms at 44100 Hz */

typedef enum {
    STATE_IDLE = 0,            /* silent, no loop recorded (loop_length == 0) */
    STATE_RECORDING = 1,
    STATE_PLAYING = 2,         /* master: continuous loop */
    STATE_OVERDUBBING = 3,
    STATE_STOP = 4,            /* silent, buffer preserved: a loop was recorded then stopped */
    STATE_WAIT_RECORDING = 5,  /* slave: waiting for tempo top to start recording */
    STATE_WAIT_PLAYING = 6,    /* slave: waiting for tempo top to play once */
    STATE_PLAYING_ONCE = 7,    /* slave: plays one pass then goes to WAIT_PLAYING */
    STATE_WAIT_ENDREC = 8      /* slave: end_record was called mid-RECORDING; still
                                   recording, waiting for next tempo top to finalize */
} t_loop_state;

typedef enum {
    REQUEST_NONE = 0,
    REQUEST_RECORD = 1,
    REQUEST_STOP_RECORDING = 2,
    REQUEST_PLAY = 3,
    REQUEST_STOP = 4,          /* end_play: target is STATE_STOP or STATE_IDLE, see perform() */
    REQUEST_OVERDUB = 5,
    REQUEST_CLEAR = 6,
    REQUEST_WAIT_RECORD = 7,
    REQUEST_WAIT_PLAY = 8,
    REQUEST_PLAY_ONCE = 9,     /* internal: triggered by tempo_tick in slave mode */
    REQUEST_TICK_FINALIZE_PLAY = 10, /* internal: armed end_record fires on next tempo tick */
    REQUEST_ARM_WAIT_ENDREC = 11,    /* internal: display WAIT_ENDREC, recording continues */
    REQUEST_CLEAR_SLAVE = 12         /* clear, but force slave mode instead of master */
} t_loop_request;

static t_class *mylooper_tilde_class = NULL;

typedef struct _mylooper_tilde {
    t_object x_obj;
    t_sample f;

    /* Audio buffer */
    t_sample *buffer;
    int buffer_size;
    int loop_length;
    int write_pos;
    int play_pos;

    /* State machine */
    t_loop_state state;
    t_float overdub_level;
    t_float feedback;

    /* Single pending request — written by message thread, read by DSP thread */
    t_loop_request pending_request;

    /* Slave only: 1 = end_record was called mid-RECORDING and is armed to
       finalize on the next tempo tick (keeps loop length sync'd to the
       master's BPM); 0 = nothing armed. */
    int armed_finalize;

    /* Sync: 1 = master (sends BPM), 0 = slave (receives BPM) */
    int master;
    t_float bpm;

    t_float sample_rate;

    /* Deferred outlet values — written by DSP, fired by clock in message thread.
       Sentinel -1 means "no pending update". */
    t_clock *x_clock;
    int     x_notify_state;
    t_float x_notify_duration;
    int     x_notify_sync_length;  /* -1 = no update; else loop_length (samples) */
    t_float x_notify_sync_bpm;     /* paired with x_notify_sync_length, display only */
    int     x_notify_master;   /* -1 = no update, 0 = slave, 1 = master */
    int     x_notify_isplaying; /* -1 = no update, else 0/1 */
    int     x_notify_isloop;    /* -1 = no update, else 0/1 */

    /* Last values sent on x_out_isplaying/x_out_isloop — compared every DSP
       block to detect edges, since loop_length can change mid-recording
       without a pending_request (see perform()). */
    int x_prev_isplaying;
    int x_prev_isloop;

    /* Outlets */
    t_outlet *x_out_signal;
    t_outlet *x_out_state;
    t_outlet *x_out_sync;      /* list (loop_length_samples bpm): sent once by the
                                   master when it starts the shared tempo */
    t_outlet *x_out_duration;
    t_outlet *x_out_master;    /* float: 1 = master, 0 = slave */
    t_outlet *x_out_isplaying; /* float: 1 unless STATE_IDLE or STATE_STOP */
    t_outlet *x_out_isloop;    /* float: 1 = loop_length > 0 */
} t_mylooper_tilde;

/* ─── Clock callback (message thread) ────────────────────────────────────── */

static void mylooper_tilde_clock_tick(t_mylooper_tilde *x)
{
    if (x->x_notify_state >= 0) {
        outlet_float(x->x_out_state, (t_float)x->x_notify_state);
        x->x_notify_state = -1;
    }
    if (x->x_notify_duration >= 0.0f) {
        outlet_float(x->x_out_duration, x->x_notify_duration);
        x->x_notify_duration = -1.0f;
    }
    if (x->x_notify_sync_length >= 0) {
        t_atom av[2];
        SETFLOAT(&av[0], (t_float)x->x_notify_sync_length);
        SETFLOAT(&av[1], x->x_notify_sync_bpm);
        outlet_list(x->x_out_sync, &s_list, 2, av);
        x->x_notify_sync_length = -1;
    }
    if (x->x_notify_master >= 0) {
        outlet_float(x->x_out_master, (t_float)x->x_notify_master);
        x->x_notify_master = -1;
    }
    if (x->x_notify_isplaying >= 0) {
        outlet_float(x->x_out_isplaying, (t_float)x->x_notify_isplaying);
        x->x_notify_isplaying = -1;
    }
    if (x->x_notify_isloop >= 0) {
        outlet_float(x->x_out_isloop, (t_float)x->x_notify_isloop);
        x->x_notify_isloop = -1;
    }
}

/* Schedule deferred outlet flush — safe to call from DSP thread */
static inline void notify(t_mylooper_tilde *x)
{
    clock_delay(x->x_clock, 0);
}

/* ─── Buffer helpers ──────────────────────────────────────────────────────── */

static void free_buffer(t_mylooper_tilde *x)
{
    if (x->buffer) { free(x->buffer); x->buffer = NULL; }
}

static int allocate_buffer(t_mylooper_tilde *x, int size)
{
    free_buffer(x);
    if (size <= 0) {
        float sr = (x->sample_rate > 0) ? x->sample_rate : 44100.0f;
        size = (int)(sr * 30.0f);
    }
    x->buffer = (t_sample *)calloc(size, sizeof(t_sample));
    if (!x->buffer) { pd_error(x, "mylooper~: out of memory"); return 0; }
    x->buffer_size = size;
    x->loop_length = x->write_pos = x->play_pos = 0;
    return 1;
}

/* Find last zero-crossing working backward from end_pos within a window.
   Returns the crossing index, or end_pos if none found. */
static int find_zero_crossing(t_sample *buf, int end_pos, int window)
{
    int limit = (end_pos > window) ? end_pos - window : 1;
    for (int i = end_pos - 1; i > limit; i--) {
        if ((buf[i-1] < 0.0f && buf[i] >= 0.0f) ||
            (buf[i-1] >= 0.0f && buf[i] < 0.0f))
            return i;
    }
    return end_pos;
}

/* Trim to nearest zero-crossing, then round up to block boundary.
   Returns new loop_length (may be up to block_size larger than old value). */
static int finalize_loop_length(t_sample *buf, int len, int block_size)
{
    len = find_zero_crossing(buf, len, 2048);
    int rem = len % block_size;
    if (rem > 0) len += block_size - rem;
    return len;
}

/* Apply short fade-in / fade-out to smooth the wrap boundary.
   Called once when recording is finalized. */
static void apply_loop_fades(t_sample *buf, int len)
{
    if (len < CROSSFADE_LEN * 2) return;
    for (int i = 0; i < CROSSFADE_LEN; i++) {
        float t = (float)i / (float)CROSSFADE_LEN;
        buf[i] *= t;
        buf[len - CROSSFADE_LEN + i] *= (1.0f - t);
    }
}

/* A recorded loop is a musical phrase of 1, 4, 8, or 16 beats — not a single
   beat. 60*sample_rate/loop_length only holds for a 1-beat loop; for longer
   loops it understates the true tempo by a factor of the beat count. Given
   that tempos in practice fall in [80,160] BPM, and the four candidate beat
   counts produce disjoint BPM ranges for any fixed loop duration (T=1 beat:
   80-160bpm -> 0.375-0.75s; 4 beats -> 1.5-3s; 8 beats -> 3-6s; 16 beats ->
   6-12s), the beat count is uniquely recoverable from the loop duration. */
static t_float compute_loop_bpm(int loop_length, t_float sample_rate)
{
    static const int beat_counts[] = { 1, 4, 8, 16 };
    t_float duration_sec = (t_float)loop_length / sample_rate;
    for (unsigned i = 0; i < sizeof(beat_counts) / sizeof(beat_counts[0]); i++) {
        t_float bpm = 60.0f * beat_counts[i] / duration_sec;
        if (bpm >= 80.0f && bpm <= 160.0f) return bpm;
    }
    /* Outside the expected range (e.g. very short test loops): fall back to
       the 1-beat assumption rather than guessing a beat count. */
    return 60.0f * sample_rate / (t_float)loop_length;
}

/* ─── DSP perform ─────────────────────────────────────────────────────────── */

static t_int *mylooper_tilde_perform(t_int *w)
{
    t_mylooper_tilde *x  = (t_mylooper_tilde *)(w[1]);
    t_sample         *in = (t_sample *)(w[2]);
    t_sample         *out = (t_sample *)(w[3]);
    int               n  = (int)(w[4]);

    t_loop_state state       = x->state;
    t_sample    *buffer      = x->buffer;
    int          buffer_size = x->buffer_size;
    int          loop_length = x->loop_length;
    int          write_pos   = x->write_pos;
    int          play_pos    = x->play_pos;
    float        overdub_level = x->overdub_level;
    float        feedback      = x->feedback;
    int          need_notify   = 0;

    /* ── Handle pending request at block boundary ── */
    if (x->pending_request != REQUEST_NONE) {
        int old_length = loop_length;

        switch (x->pending_request) {
            case REQUEST_RECORD:
                if (buffer) memset(buffer, 0, buffer_size * sizeof(t_sample));
                loop_length = write_pos = play_pos = 0;
                state = STATE_RECORDING;
                x->armed_finalize = 0;
                x->x_notify_state = (int)state;
                need_notify = 1;
                break;

            case REQUEST_STOP_RECORDING:
                if (loop_length > 0) {
                    loop_length = finalize_loop_length(buffer, loop_length, n);
                    if (loop_length > buffer_size) loop_length = buffer_size;
                    for (int i = old_length; i < loop_length; i++) buffer[i] = 0.0f;
                    apply_loop_fades(buffer, loop_length);
                }
                /* master: play the first pass once, then join the tick-driven
                   regime like everyone else (STATE_WAIT_PLAYING at the end of
                   that pass) instead of looping freely — this is also the
                   moment the shared external tempo is born, so it's reported
                   once on x_out_sync (loop_length bpm) to start the metronome.
                   slave goes straight to WAIT_PLAYING so its first play is
                   synced to the already-running tempo. */
                if (x->master) {
                    if (loop_length > 0) {
                        state = STATE_PLAYING_ONCE;
                        play_pos = 0;
                    } else {
                        state = STATE_IDLE;
                    }
                } else {
                    state = STATE_WAIT_PLAYING;
                }
                if (loop_length > 0 && x->sample_rate > 0) {
                    x->x_notify_duration = (t_float)loop_length / x->sample_rate * 1000.0f;
                    if (x->master) {
                        x->x_notify_sync_length = loop_length;
                        x->x_notify_sync_bpm = compute_loop_bpm(loop_length, x->sample_rate);
                    }
                }
                x->x_notify_state = (int)state;
                need_notify = 1;
                break;

            case REQUEST_PLAY:
                /* keep position when returning from overdub, reset otherwise */
                if (state != STATE_OVERDUBBING)
                    play_pos = 0;
                state = STATE_PLAYING;
                if (loop_length > 0 && x->sample_rate > 0)
                    x->x_notify_duration = (t_float)loop_length / x->sample_rate * 1000.0f;
                x->x_notify_state = (int)state;
                need_notify = 1;
                break;

            case REQUEST_STOP:
                /* STATE_STOP when a loop already exists, STATE_IDLE only when
                   nothing has ever been recorded (or after clear) */
                state = (loop_length > 0) ? STATE_STOP : STATE_IDLE;
                x->armed_finalize = 0;
                x->x_notify_state = (int)state;
                need_notify = 1;
                break;

            case REQUEST_OVERDUB:
                /* position is always kept — seamless transition from whatever
                   state we were in (PLAYING, IDLE with a finished pass, ...) */
                state = STATE_OVERDUBBING;
                if (loop_length > 0 && x->sample_rate > 0)
                    x->x_notify_duration = (t_float)loop_length / x->sample_rate * 1000.0f;
                x->x_notify_state = (int)state;
                need_notify = 1;
                break;

            case REQUEST_CLEAR:
            case REQUEST_CLEAR_SLAVE:
                /* "clear" resets to master (a new tempo can only be born from
                   master mode); "clear slave" resets into slave mode directly,
                   e.g. to pre-arm an instance that joins an already-running
                   tempo without ever having recorded the first loop itself. */
                if (buffer) memset(buffer, 0, buffer_size * sizeof(t_sample));
                loop_length = write_pos = play_pos = 0;
                state = STATE_IDLE;
                x->master = (x->pending_request == REQUEST_CLEAR) ? 1 : 0;
                x->armed_finalize = 0;
                x->x_notify_state = (int)state;
                x->x_notify_duration = 0.0f;
                x->x_notify_master = x->master;
                need_notify = 1;
                break;

            case REQUEST_WAIT_RECORD:
                state = STATE_WAIT_RECORDING;
                x->x_notify_state = (int)state;
                need_notify = 1;
                break;

            case REQUEST_WAIT_PLAY:
                state = STATE_WAIT_PLAYING;
                x->x_notify_state = (int)state;
                need_notify = 1;
                break;

            case REQUEST_PLAY_ONCE:
                play_pos = 0;
                state = STATE_PLAYING_ONCE;
                x->x_notify_state = (int)state;
                need_notify = 1;
                break;

            case REQUEST_ARM_WAIT_ENDREC:
                /* recording keeps running (see STATE_WAIT_ENDREC below) — only
                   the reported state changes, until the tempo tick finalizes it */
                state = STATE_WAIT_ENDREC;
                x->x_notify_state = (int)state;
                need_notify = 1;
                break;

            case REQUEST_TICK_FINALIZE_PLAY:
                /* fired by tempo_tick exactly on the tick: recording so far is
                   already an exact multiple of the master's loop length */
                if (loop_length > 0) {
                    loop_length = finalize_loop_length(buffer, loop_length, n);
                    if (loop_length > buffer_size) loop_length = buffer_size;
                    for (int i = old_length; i < loop_length; i++) buffer[i] = 0.0f;
                    apply_loop_fades(buffer, loop_length);
                    if (x->sample_rate > 0)
                        x->x_notify_duration = (t_float)loop_length / x->sample_rate * 1000.0f;
                }
                play_pos = 0;
                state = STATE_PLAYING_ONCE;
                x->x_notify_state = (int)state;
                need_notify = 1;
                break;

            case REQUEST_NONE:
                break;
        }
        x->pending_request = REQUEST_NONE;
    }

    if (!buffer) {
        while (n--) *out++ = *in++;
        return (w + 5);
    }

    /* ── Sample loop ── */
    for (int i = 0; i < n; i++) {
        t_sample input  = *in++;
        t_sample output = 0.0f;

        switch (state) {
            case STATE_IDLE:
                break;

            case STATE_RECORDING:
            case STATE_WAIT_ENDREC:
                /* still capturing input either way — WAIT_ENDREC only differs
                   in what's reported on the state outlet */
                if (write_pos < buffer_size) {
                    buffer[write_pos++] = input;
                    loop_length = write_pos;
                } else {
                    /* Buffer full: finalize and start playing. Same genesis
                       event as REQUEST_STOP_RECORDING above — master plays
                       the first pass once then joins the tick-driven regime,
                       slave waits for the (already running) tempo's tick. */
                    apply_loop_fades(buffer, loop_length);
                    state = x->master ? STATE_PLAYING_ONCE : STATE_WAIT_PLAYING;
                    play_pos = 0;
                    x->x_notify_state = (int)state;
                    if (x->sample_rate > 0) {
                        x->x_notify_duration = (t_float)loop_length / x->sample_rate * 1000.0f;
                        if (x->master) {
                            x->x_notify_sync_length = loop_length;
                            x->x_notify_sync_bpm = compute_loop_bpm(loop_length, x->sample_rate);
                        }
                    }
                    need_notify = 1;
                }
                break;

            case STATE_PLAYING:
                if (loop_length > 0) {
                    output = buffer[play_pos++];
                    if (play_pos >= loop_length) play_pos = 0;
                }
                break;

            case STATE_OVERDUBBING:
                if (loop_length > 0) {
                    output = buffer[play_pos];
                    buffer[play_pos] = buffer[play_pos] * feedback + input * overdub_level;
                    play_pos++;
                    if (play_pos >= loop_length) play_pos = 0;
                } else {
                    /* Nothing recorded yet: record directly */
                    if (write_pos < buffer_size) {
                        buffer[write_pos++] = input;
                        loop_length = write_pos;
                    }
                    output = input;
                }
                break;

            case STATE_STOP:
                break;

            case STATE_WAIT_RECORDING:
                /* Play existing loop while waiting for sync trigger */
                if (loop_length > 0) {
                    output = buffer[play_pos++];
                    if (play_pos >= loop_length) play_pos = 0;
                }
                break;

            case STATE_WAIT_PLAYING:
                break;

            case STATE_PLAYING_ONCE:
                if (loop_length > 0 && play_pos < loop_length) {
                    output = buffer[play_pos++];
                    if (play_pos >= loop_length) {
                        /* pass finished: loop_length is still > 0, so this is
                           STATE_WAIT_PLAYING (armed, waiting for the next tick
                           to replay), not STATE_IDLE (which means no loop) */
                        play_pos = 0;
                        state = STATE_WAIT_PLAYING;
                        x->x_notify_state = (int)state;
                        need_notify = 1;
                    }
                }
                break;
        }

        *out++ = output;
    }

    x->state      = state;
    x->write_pos  = write_pos;
    x->play_pos   = play_pos;
    x->loop_length = loop_length;

    /* isPlaying/isLoopPresent can change without a pending_request (e.g.
       loop_length grows sample-by-sample while recording), so re-derive and
       diff against the last reported value on every block rather than only
       inside the switch above. */
    int is_playing = (state != STATE_IDLE && state != STATE_STOP);
    int is_loop    = (loop_length > 0);
    if (is_playing != x->x_prev_isplaying) {
        x->x_prev_isplaying   = is_playing;
        x->x_notify_isplaying = is_playing;
        need_notify = 1;
    }
    if (is_loop != x->x_prev_isloop) {
        x->x_prev_isloop   = is_loop;
        x->x_notify_isloop = is_loop;
        need_notify = 1;
    }

    if (need_notify) notify(x);

    return (w + 5);
}

static void mylooper_tilde_dsp(t_mylooper_tilde *x, t_signal **sp)
{
    x->sample_rate = sp[0]->s_sr;
    dsp_add(mylooper_tilde_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
}

/* ─── Message handlers ────────────────────────────────────────────────────── */

static void mylooper_tilde_record(t_mylooper_tilde *x)
{
    /* In slave mode, arm for sync — actual recording starts on next tempo top */
    x->pending_request = x->master ? REQUEST_RECORD : REQUEST_WAIT_RECORD;
}

static void mylooper_tilde_end_record(t_mylooper_tilde *x)
{
    if (x->state != STATE_RECORDING) return;
    if (x->master)
        x->pending_request = REQUEST_STOP_RECORDING;
    else {
        /* wait for the next tempo tick so the loop stays sync'd to the master;
           recording keeps running, only the reported state changes meanwhile */
        x->armed_finalize = 1;
        x->pending_request = REQUEST_ARM_WAIT_ENDREC;
    }
}

/* Recording only ever ends via end_record — play/overdub/wait_play all
   require a finalized loop, so they're rejected while still recording. */
static void mylooper_tilde_play(t_mylooper_tilde *x)
{
    if (x->state == STATE_RECORDING || x->state == STATE_WAIT_ENDREC) {
        pd_error(x, "mylooper~: cannot play while recording, call end_record first");
        return;
    }
    if (x->loop_length > 0) {
        if (x->state == STATE_OVERDUBBING)
            /* already sync'd and running continuously — resume playback
               immediately, no need to wait for a tempo tick */
            x->pending_request = REQUEST_PLAY;
        else
            x->pending_request = x->master ? REQUEST_PLAY : REQUEST_WAIT_PLAY;
    } else {
        pd_error(x, "mylooper~: nothing to play (record first)");
    }
}

static void mylooper_tilde_end_play(t_mylooper_tilde *x)
{
    x->pending_request = REQUEST_STOP;
}

static void mylooper_tilde_overdub(t_mylooper_tilde *x)
{
    /* overdub only makes sense on a loop that's actively playing */
    if (x->state != STATE_PLAYING && x->state != STATE_PLAYING_ONCE &&
        x->state != STATE_OVERDUBBING) {
        pd_error(x, "mylooper~: overdub requires the loop to be playing, call play first");
        return;
    }
    x->pending_request = REQUEST_OVERDUB;
}

static void mylooper_tilde_end_overdub(t_mylooper_tilde *x)
{
    if (x->state != STATE_OVERDUBBING) {
        pd_error(x, "mylooper~: not overdubbing");
        return;
    }
    /* already sync'd and running continuously — resume playback immediately,
       on master and slave alike, no need to wait for a tempo tick */
    x->pending_request = REQUEST_PLAY;
}

/* "clear" -> master reset (default). "clear slave" -> reset directly into
   slave mode, so the instance is pre-armed to join a tempo it never had to
   record the first loop for. Any other/no argument falls back to "clear". */
static void mylooper_tilde_clear(t_mylooper_tilde *x, t_symbol *s, int argc, t_atom *argv)
{
    (void)s;
    if (argc >= 1 && argv[0].a_type == A_SYMBOL &&
        argv[0].a_w.w_symbol == gensym("slave"))
        x->pending_request = REQUEST_CLEAR_SLAVE;
    else
        x->pending_request = REQUEST_CLEAR;
}

static void mylooper_tilde_wait_record(t_mylooper_tilde *x)
{
    x->pending_request = REQUEST_WAIT_RECORD;
}

static void mylooper_tilde_wait_play(t_mylooper_tilde *x)
{
    if (x->state == STATE_RECORDING || x->state == STATE_WAIT_ENDREC) {
        pd_error(x, "mylooper~: cannot wait_play while recording, call end_record first");
        return;
    }
    x->pending_request = REQUEST_WAIT_PLAY;
}

/* Tempo inlet: receives a bare tick (bang) from the shared external
   metronome (see Sync system in CLAUDE.md) — the tempo value itself was
   already sent once, out-of-band, on x_out_sync when the tempo was born.
   - Switches this instance to slave mode on first reception.
   - Triggers armed wait states and keeps slave loops alive after each pass. */
static void tempo_tick(t_mylooper_tilde *x)
{
    if (x->master) {
        x->master = 0;
        x->x_notify_master = 0;
        clock_delay(x->x_clock, 0);
    }

    /* x->state is only updated by _perform() at the start of a DSP block, so
       a WAIT_PLAY/WAIT_RECORD request written by play()/wait_play()/record()
       earlier in the SAME block hasn't landed in x->state yet. Without this,
       a tick arriving in that same block would read the stale pre-wait state,
       match no branch below, and be silently dropped — forcing a full extra
       tempo cycle before the arm is actually consumed. Treat a still-pending
       wait request as if it had already applied. */
    t_loop_state effective_state = x->state;
    if (x->pending_request == REQUEST_WAIT_PLAY)
        effective_state = STATE_WAIT_PLAYING;
    else if (x->pending_request == REQUEST_WAIT_RECORD)
        effective_state = STATE_WAIT_RECORDING;

    if ((effective_state == STATE_RECORDING || effective_state == STATE_WAIT_ENDREC) && x->armed_finalize) {
        /* end_record was requested mid-recording: only now, on the tick, do we
           actually cut the recording — so its length is an exact multiple of
           the master's loop length */
        x->pending_request = REQUEST_TICK_FINALIZE_PLAY;
        x->armed_finalize = 0;
    }
    else if (effective_state == STATE_WAIT_RECORDING)
        x->pending_request = REQUEST_RECORD;
    else if (effective_state == STATE_WAIT_PLAYING)
        /* covers both an explicit wait_play arm and a play_once pass that
           just finished (which now also lands in STATE_WAIT_PLAYING) */
        if (x->loop_length > 0)
            x->pending_request = REQUEST_PLAY_ONCE;
}

static void mylooper_tilde_tempo_tick(t_mylooper_tilde *x)
{
    tempo_tick(x);
}

/* Direct-message equivalent, kept for manual testing without a real
   metronome (e.g. a message box) — the float value is stored for reference
   only (x->bpm is never read elsewhere) and otherwise behaves as a tick. */
static void mylooper_tilde_tempo_in(t_mylooper_tilde *x, t_floatarg bpm)
{
    x->bpm = bpm;
    tempo_tick(x);
}

/* ─── Parameter setters ───────────────────────────────────────────────────── */

static void mylooper_tilde_overdub_level(t_mylooper_tilde *x, t_floatarg v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    x->overdub_level = v;
}

static void mylooper_tilde_feedback(t_mylooper_tilde *x, t_floatarg v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    x->feedback = v;
}

/* ─── Constructor / Destructor ───────────────────────────────────────────── */

static void mylooper_tilde_free(t_mylooper_tilde *x)
{
    clock_free(x->x_clock);
    free_buffer(x);
}

static void *mylooper_tilde_new(void)
{
    t_mylooper_tilde *x = (t_mylooper_tilde *)pd_new(mylooper_tilde_class);

    x->buffer      = NULL;
    x->buffer_size = 0;
    x->loop_length = x->write_pos = x->play_pos = 0;

    x->state        = STATE_IDLE;
    x->overdub_level = 0.8f;
    x->feedback      = 0.95f;
    x->master        = 1;
    x->bpm           = 120.0f;
    x->sample_rate   = 44100.0f;

    x->pending_request = REQUEST_NONE;
    x->armed_finalize  = 0;

    x->x_clock           = clock_new(x, (t_method)mylooper_tilde_clock_tick);
    x->x_notify_state    = (int)STATE_IDLE;
    x->x_notify_duration = 0.0f;
    x->x_notify_sync_length = -1;
    x->x_notify_sync_bpm    = 0.0f;
    x->x_notify_master   = 1;  /* start as master */
    x->x_notify_isplaying = 0; /* STATE_IDLE at construction */
    x->x_notify_isloop    = 0; /* loop_length == 0 at construction */
    x->x_prev_isplaying   = 0;
    x->x_prev_isloop      = 0;

    allocate_buffer(x, 0);

    /* Control messages inlet (2nd inlet) */
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, 0, 0);
    /* Tempo/sync inlet (3rd inlet): bang tick from the shared metronome */
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_bang, gensym("tempo_tick"));

    x->x_out_signal   = outlet_new(&x->x_obj, &s_signal);
    x->x_out_state    = outlet_new(&x->x_obj, &s_float);
    x->x_out_sync     = outlet_new(&x->x_obj, &s_list);
    x->x_out_duration = outlet_new(&x->x_obj, &s_float);
    x->x_out_master   = outlet_new(&x->x_obj, &s_float);
    x->x_out_isplaying = outlet_new(&x->x_obj, &s_float);
    x->x_out_isloop    = outlet_new(&x->x_obj, &s_float);

    /* Send initial state/duration via clock so it fires after construction */
    clock_delay(x->x_clock, 0);

    return (void *)x;
}

/* ─── Setup ───────────────────────────────────────────────────────────────── */

void mylooper_tilde_setup(void)
{
    mylooper_tilde_class = class_new(gensym("mylooper~"),
                                     (t_newmethod)mylooper_tilde_new,
                                     (t_method)mylooper_tilde_free,
                                     sizeof(t_mylooper_tilde),
                                     CLASS_DEFAULT,
                                     0);

    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_dsp,
                    gensym("dsp"), A_CANT, 0);

    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_record,
                    gensym("record"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_end_record,
                    gensym("end_record"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_play,
                    gensym("play"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_end_play,
                    gensym("end_play"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_overdub,
                    gensym("overdub"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_end_overdub,
                    gensym("end_overdub"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_clear,
                    gensym("clear"), A_GIMME, 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_wait_record,
                    gensym("wait_record"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_wait_play,
                    gensym("wait_play"), 0);

    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_overdub_level,
                    gensym("overdub_level"), A_FLOAT, 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_feedback,
                    gensym("feedback"), A_FLOAT, 0);

    /* tempo_tick is invoked via the 3rd inlet's bang routing (the shared
       metronome). tempo_in is a float-taking direct-message equivalent, kept
       for manual testing (e.g. from a message box) without a real metronome. */
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_tempo_tick,
                    gensym("tempo_tick"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_tempo_in,
                    gensym("tempo_in"), A_FLOAT, 0);

    CLASS_MAINSIGNALIN(mylooper_tilde_class, t_mylooper_tilde, f);
}
