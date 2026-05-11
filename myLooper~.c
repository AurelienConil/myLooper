#include "m_pd.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define CROSSFADE_LEN 256  /* ~5ms at 44100 Hz */

typedef enum {
    STATE_IDLE = 0,
    STATE_RECORDING = 1,
    STATE_PLAYING = 2,         /* master: continuous loop */
    STATE_OVERDUBBING = 3,
    STATE_PAUSED = 4,
    STATE_WAIT_RECORDING = 5,  /* slave: waiting for tempo top to start recording */
    STATE_WAIT_PLAYING = 6,    /* slave: waiting for tempo top to play once */
    STATE_PLAYING_ONCE = 7,    /* slave: plays one pass then goes to IDLE */
    STATE_WAIT_OVERDUBBING = 8 /* slave: plays loop, waits for BPM tick to start overdub */
} t_loop_state;

typedef enum {
    REQUEST_NONE = 0,
    REQUEST_RECORD = 1,
    REQUEST_STOP_RECORDING = 2,
    REQUEST_PLAY = 3,
    REQUEST_STOP = 4,
    REQUEST_OVERDUB = 5,
    REQUEST_PAUSE = 6,
    REQUEST_CLEAR = 7,
    REQUEST_WAIT_RECORD = 8,
    REQUEST_WAIT_PLAY = 9,
    REQUEST_PLAY_ONCE = 10,    /* internal: triggered by tempo_in in slave mode */
    REQUEST_WAIT_OVERDUB = 11  /* slave: finalize recording, wait for BPM tick to overdub */
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

    /* Sync: 1 = master (sends BPM), 0 = slave (receives BPM) */
    int master;
    t_float bpm;

    t_float sample_rate;

    /* Deferred outlet values — written by DSP, fired by clock in message thread.
       Sentinel -1 means "no pending update". */
    t_clock *x_clock;
    int     x_notify_state;
    t_float x_notify_duration;
    t_float x_notify_bpm;
    int     x_notify_master;   /* -1 = no update, 0 = slave, 1 = master */

    /* Outlets */
    t_outlet *x_out_signal;
    t_outlet *x_out_state;
    t_outlet *x_out_sync;      /* float: BPM sent by master on each loop wrap */
    t_outlet *x_out_duration;
    t_outlet *x_out_master;    /* float: 1 = master, 0 = slave */
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
    if (x->x_notify_bpm >= 0.0f) {
        outlet_float(x->x_out_sync, x->x_notify_bpm);
        x->x_notify_bpm = -1.0f;
    }
    if (x->x_notify_master >= 0) {
        outlet_float(x->x_out_master, (t_float)x->x_notify_master);
        x->x_notify_master = -1;
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
                /* slave goes to WAIT_PLAYING so the first play is synced to tempo */
                state = x->master ? STATE_IDLE : STATE_WAIT_PLAYING;
                if (loop_length > 0 && x->sample_rate > 0)
                    x->x_notify_duration = (t_float)loop_length / x->sample_rate * 1000.0f;
                x->x_notify_state = (int)state;
                need_notify = 1;
                break;

            case REQUEST_PLAY:
                if (state == STATE_RECORDING && loop_length > 0) {
                    /* finalize recording before playing */
                    loop_length = finalize_loop_length(buffer, loop_length, n);
                    if (loop_length > buffer_size) loop_length = buffer_size;
                    for (int i = old_length; i < loop_length; i++) buffer[i] = 0.0f;
                    apply_loop_fades(buffer, loop_length);
                }
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
                state = STATE_IDLE;
                x->x_notify_state = (int)state;
                need_notify = 1;
                break;

            case REQUEST_OVERDUB:
                if (state == STATE_RECORDING && loop_length > 0) {
                    loop_length = finalize_loop_length(buffer, loop_length, n);
                    if (loop_length > buffer_size) loop_length = buffer_size;
                    for (int i = old_length; i < loop_length; i++) buffer[i] = 0.0f;
                    apply_loop_fades(buffer, loop_length);
                    play_pos = 0;
                } else if (state == STATE_WAIT_OVERDUBBING) {
                    play_pos = 0;  /* snap to loop start on BPM tick */
                }
                /* when coming from PLAYING, play_pos is kept — seamless transition */
                state = STATE_OVERDUBBING;
                if (loop_length > 0 && x->sample_rate > 0)
                    x->x_notify_duration = (t_float)loop_length / x->sample_rate * 1000.0f;
                x->x_notify_state = (int)state;
                need_notify = 1;
                break;

            case REQUEST_PAUSE:
                state = STATE_PAUSED;
                x->x_notify_state = (int)state;
                need_notify = 1;
                break;

            case REQUEST_CLEAR:
                if (buffer) memset(buffer, 0, buffer_size * sizeof(t_sample));
                loop_length = write_pos = play_pos = 0;
                state = STATE_IDLE;
                x->master = 1;
                x->x_notify_state = (int)state;
                x->x_notify_duration = 0.0f;
                x->x_notify_master = 1;
                need_notify = 1;
                break;

            case REQUEST_WAIT_RECORD:
                state = STATE_WAIT_RECORDING;
                x->x_notify_state = (int)state;
                need_notify = 1;
                break;

            case REQUEST_WAIT_PLAY:
                if (state == STATE_RECORDING && loop_length > 0) {
                    loop_length = finalize_loop_length(buffer, loop_length, n);
                    if (loop_length > buffer_size) loop_length = buffer_size;
                    for (int i = old_length; i < loop_length; i++) buffer[i] = 0.0f;
                    apply_loop_fades(buffer, loop_length);
                    if (x->sample_rate > 0)
                        x->x_notify_duration = (t_float)loop_length / x->sample_rate * 1000.0f;
                }
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

            case REQUEST_WAIT_OVERDUB:
                if (state == STATE_RECORDING && loop_length > 0) {
                    loop_length = finalize_loop_length(buffer, loop_length, n);
                    if (loop_length > buffer_size) loop_length = buffer_size;
                    for (int i = old_length; i < loop_length; i++) buffer[i] = 0.0f;
                    apply_loop_fades(buffer, loop_length);
                    if (x->sample_rate > 0)
                        x->x_notify_duration = (t_float)loop_length / x->sample_rate * 1000.0f;
                    play_pos = 0;
                }
                state = STATE_WAIT_OVERDUBBING;
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
                if (write_pos < buffer_size) {
                    buffer[write_pos++] = input;
                    loop_length = write_pos;
                } else {
                    /* Buffer full: finalize and start playing */
                    apply_loop_fades(buffer, loop_length);
                    state = STATE_PLAYING;
                    play_pos = 0;
                    x->x_notify_state = (int)state;
                    if (x->sample_rate > 0)
                        x->x_notify_duration = (t_float)loop_length / x->sample_rate * 1000.0f;
                    need_notify = 1;
                }
                break;

            case STATE_PLAYING:
                if (loop_length > 0) {
                    output = buffer[play_pos++];
                    if (play_pos >= loop_length) {
                        play_pos = 0;
                        if (x->master && x->sample_rate > 0) {
                            x->x_notify_bpm = 60.0f * x->sample_rate / (float)loop_length;
                            need_notify = 1;
                        }
                    }
                }
                break;

            case STATE_OVERDUBBING:
                if (loop_length > 0) {
                    output = buffer[play_pos];
                    buffer[play_pos] = buffer[play_pos] * feedback + input * overdub_level;
                    play_pos++;
                    if (play_pos >= loop_length) {
                        play_pos = 0;
                        if (x->master && x->sample_rate > 0) {
                            x->x_notify_bpm = 60.0f * x->sample_rate / (float)loop_length;
                            need_notify = 1;
                        }
                    }
                } else {
                    /* Nothing recorded yet: record directly */
                    if (write_pos < buffer_size) {
                        buffer[write_pos++] = input;
                        loop_length = write_pos;
                    }
                    output = input;
                }
                break;

            case STATE_PAUSED:
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

            case STATE_WAIT_OVERDUBBING:
                /* play the loop while waiting for the BPM tick to start overdubbing */
                if (loop_length > 0) {
                    output = buffer[play_pos++];
                    if (play_pos >= loop_length) play_pos = 0;
                }
                break;

            case STATE_PLAYING_ONCE:
                if (loop_length > 0 && play_pos < loop_length) {
                    output = buffer[play_pos++];
                    if (play_pos >= loop_length) {
                        play_pos = 0;
                        state = STATE_IDLE;
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

static void mylooper_tilde_stop_recording(t_mylooper_tilde *x)
{
    if (x->state == STATE_RECORDING)
        x->pending_request = REQUEST_STOP_RECORDING;
}

static void mylooper_tilde_play(t_mylooper_tilde *x)
{
    if (x->state == STATE_RECORDING || x->loop_length > 0)
        x->pending_request = x->master ? REQUEST_PLAY : REQUEST_WAIT_PLAY;
    else
        pd_error(x, "mylooper~: nothing to play (record first)");
}

static void mylooper_tilde_stop(t_mylooper_tilde *x)
{
    x->pending_request = REQUEST_STOP;
}

static void mylooper_tilde_overdub(t_mylooper_tilde *x)
{
    if (x->state == STATE_RECORDING || x->loop_length > 0) {
        /* slave coming from recording: finalize and wait for BPM tick to start overdub */
        if (!x->master && x->state == STATE_RECORDING)
            x->pending_request = REQUEST_WAIT_OVERDUB;
        else
            x->pending_request = REQUEST_OVERDUB;
    } else {
        pd_error(x, "mylooper~: nothing to overdub (record first)");
    }
}

static void mylooper_tilde_pause(t_mylooper_tilde *x)
{
    x->pending_request = REQUEST_PAUSE;
}

static void mylooper_tilde_clear(t_mylooper_tilde *x)
{
    x->pending_request = REQUEST_CLEAR;
}

static void mylooper_tilde_wait_record(t_mylooper_tilde *x)
{
    x->pending_request = REQUEST_WAIT_RECORD;
}

static void mylooper_tilde_wait_play(t_mylooper_tilde *x)
{
    x->pending_request = REQUEST_WAIT_PLAY;
}

/* Tempo inlet: receives BPM float from a master looper.
   - Switches this instance to slave mode on first reception.
   - Triggers armed wait states and keeps slave loops alive after each pass. */
static void mylooper_tilde_tempo_in(t_mylooper_tilde *x, t_floatarg bpm)
{
    x->bpm = bpm;
    if (x->master) {
        x->master = 0;
        x->x_notify_master = 0;
        clock_delay(x->x_clock, 0);
    }
    if (x->state == STATE_WAIT_RECORDING)
        x->pending_request = REQUEST_RECORD;
    else if (x->state == STATE_WAIT_OVERDUBBING)
        x->pending_request = REQUEST_OVERDUB;
    else if (x->state == STATE_WAIT_PLAYING || x->state == STATE_IDLE)
        /* IDLE means a previous play_once just finished — retrigger to keep looping */
        if (x->loop_length > 0)
            x->pending_request = REQUEST_PLAY_ONCE;
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

    x->x_clock           = clock_new(x, (t_method)mylooper_tilde_clock_tick);
    x->x_notify_state    = (int)STATE_IDLE;
    x->x_notify_duration = 0.0f;
    x->x_notify_bpm      = -1.0f;
    x->x_notify_master   = 1;  /* start as master */

    allocate_buffer(x, 0);

    /* Control messages inlet (2nd inlet) */
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, 0, 0);
    /* Tempo/sync inlet (3rd inlet): float BPM from a master looper */
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_float, gensym("tempo_in"));

    x->x_out_signal   = outlet_new(&x->x_obj, &s_signal);
    x->x_out_state    = outlet_new(&x->x_obj, &s_float);
    x->x_out_sync     = outlet_new(&x->x_obj, &s_float);
    x->x_out_duration = outlet_new(&x->x_obj, &s_float);
    x->x_out_master   = outlet_new(&x->x_obj, &s_float);

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
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_stop_recording,
                    gensym("stop_recording"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_play,
                    gensym("play"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_stop,
                    gensym("stop"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_overdub,
                    gensym("overdub"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_pause,
                    gensym("pause"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_clear,
                    gensym("clear"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_wait_record,
                    gensym("wait_record"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_wait_play,
                    gensym("wait_play"), 0);

    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_overdub_level,
                    gensym("overdub_level"), A_FLOAT, 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_feedback,
                    gensym("feedback"), A_FLOAT, 0);

    /* tempo_in is invoked via the second inlet routing AND directly as a message */
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_tempo_in,
                    gensym("tempo_in"), A_FLOAT, 0);

    CLASS_MAINSIGNALIN(mylooper_tilde_class, t_mylooper_tilde, f);
}
