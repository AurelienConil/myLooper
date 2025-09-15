#include "m_pd.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

// Machine state enumeration
typedef enum {
    STATE_IDLE = 0,
    STATE_RECORDING = 1, 
    STATE_PLAYING = 2,
    STATE_OVERDUBBING = 3,
    STATE_PAUSED = 4,
    STATE_WAITING_SYNC = 5,
    STATE_PLAYING_ONCE = 6
} t_loop_state;

// Request type enumeration for thread-safe state transitions
typedef enum {
    REQUEST_NONE = 0,
    REQUEST_RECORD = 1,
    REQUEST_STOP_RECORDING = 2, 
    REQUEST_PLAY = 3,
    REQUEST_PLAY_ONCE = 4,
    REQUEST_STOP = 5,
    REQUEST_OVERDUB = 6,
    REQUEST_PAUSE = 7,
    REQUEST_CLEAR = 8,
    REQUEST_WAIT_SYNC = 9
} t_loop_request;

static t_class *mylooper_tilde_class = NULL;

typedef struct _mylooper_tilde
{
    t_object x_obj;
    t_sample f; // dummy variable for signal inlet
    
    // Audio buffer
    t_sample *buffer;
    int buffer_size;      // total allocated size
    int loop_length;      // actual loop length in samples
    int write_pos;        // current write position
    int play_pos;         // current play position
    
    // Loop parameters
    t_loop_state state;   // current state of the looper
    t_float overdub_level;// level for overdubbing (0.0 to 1.0)
    t_float feedback;     // feedback level for overdub (0.0 to 1.0)
    
    // Request flags (for thread-safe state transitions)
    t_loop_request pending_request;  // Pending state change request
    int play_pos_reset;              // Flag to reset play position on next cycle
    int just_finished_playonce;      // Flag to track if we just finished a play_once
    
    // Sync parameters
    int sync_enabled;     // whether sync is enabled
    t_float bpm;          // beats per minute
    int beats_per_loop;   // number of beats in the loop
    int master;           // is this looper the master (1) or slave (0)
    t_float sync_phase;   // current phase in the sync cycle (0.0 to 1.0)
    
    // Internal state
    t_float sample_rate;  // current sample rate
    t_float elapsed_time; // elapsed time in seconds
    
    // Outlets
    t_outlet *x_out_signal;    // signal outlet
    t_outlet *x_out_state;     // state outlet (0-5)
    t_outlet *x_out_sync;      // sync outlet for master mode
    t_outlet *x_out_duration;  // duration outlet (loop length in ms)
    
} t_mylooper_tilde;

// Forward declarations
static void mylooper_tilde_clear(t_mylooper_tilde *x);
static void mylooper_tilde_record(t_mylooper_tilde *x);
static void mylooper_tilde_stop_recording(t_mylooper_tilde *x);
static void mylooper_tilde_play(t_mylooper_tilde *x);
static void mylooper_tilde_play_once(t_mylooper_tilde *x);
static void mylooper_tilde_stop(t_mylooper_tilde *x);
static void mylooper_tilde_overdub(t_mylooper_tilde *x);
static void mylooper_tilde_pause(t_mylooper_tilde *x);

// Update state and send state notification
static void update_state(t_mylooper_tilde *x, t_loop_state new_state) {
    x->state = new_state;
    outlet_float(x->x_out_state, (t_float)new_state);
}

// Calculate and output loop duration in milliseconds
static void output_loop_duration(t_mylooper_tilde *x) {
    if (x->loop_length > 0 && x->sample_rate > 0) {
        t_float duration_ms = (t_float)x->loop_length / x->sample_rate * 1000.0f;
        outlet_float(x->x_out_duration, duration_ms);
        post("mylooper~: loop duration = %.2f ms", duration_ms);
    } else {
        outlet_float(x->x_out_duration, 0.0f);
    }
}

// Free buffer memory
static void free_buffer(t_mylooper_tilde *x) {
    if (x->buffer) {
        free(x->buffer);
        x->buffer = NULL;
    }
}

// Allocate or reallocate buffer memory
static int allocate_buffer(t_mylooper_tilde *x, int size) {
    // Free existing buffer if any
    free_buffer(x);
    
    // Calculate buffer size in samples
    // Use a default of 10 seconds if sample rate is not yet known
    int buffer_size = size;
    if (buffer_size <= 0) {
        float sr = (x->sample_rate > 0) ? x->sample_rate : 44100.0f;
        buffer_size = (int)(sr * 30.0f); // Default 30 seconds max
    }
    
    // Allocate and initialize buffer
    x->buffer = (t_sample *)calloc(buffer_size, sizeof(t_sample));
    if (!x->buffer) {
        pd_error(x, "mylooper~: could not allocate buffer memory");
        return 0;
    }
    
    x->buffer_size = buffer_size;
    x->loop_length = 0;
    x->write_pos = 0;
    x->play_pos = 0;
    
    return 1;
}

// DSP perform routine
static t_int *mylooper_tilde_perform(t_int *w)
{
    t_mylooper_tilde *x = (t_mylooper_tilde *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *out = (t_sample *)(w[3]);
    int n = (int)(w[4]);
    
    // Local variables
    t_loop_state state = x->state;
    t_sample *buffer = x->buffer;
    int buffer_size = x->buffer_size;
    int loop_length = x->loop_length;
    int write_pos = x->write_pos;
    int play_pos = x->play_pos;
    float overdub_level = x->overdub_level;
    float feedback = x->feedback;
    
    // CRITICAL: Handle any pending requests at the beginning of the DSP cycle
    // This ensures clean state transitions synchronized with audio processing
    if (x->pending_request != REQUEST_NONE) {
        switch (x->pending_request) {
            case REQUEST_NONE:
                // Nothing to do
                break;
                
            case REQUEST_RECORD:
                // Clear buffer first
                if (buffer) {
                    memset(buffer, 0, buffer_size * sizeof(t_sample));
                }
                loop_length = 0;
                write_pos = 0;
                play_pos = 0;
                state = STATE_RECORDING;
                post("mylooper~: recording started in DSP thread");
                break;
                
            case REQUEST_STOP_RECORDING:
                // Make sure loop length is a multiple of the buffer size (n)
                if (loop_length > 0) {
                    int remainder = loop_length % n;
                    if (remainder > 0) {
                        // Round up to the next multiple of n
                        int additional = n - remainder;
                        if ((loop_length + additional) <= buffer_size) {
                            // Zero out the additional samples
                            for (int i = loop_length; i < loop_length + additional; i++) {
                                buffer[i] = 0.0f;
                            }
                            loop_length += additional;
                            post("mylooper~: adjusted loop length to %d samples to ensure it's a multiple of buffer size %d", loop_length, n);
                        }
                    }
                }
                state = STATE_IDLE;
                
                // Output loop duration
                if (loop_length > 0 && x->sample_rate > 0) {
                    t_float duration_ms = (t_float)loop_length / x->sample_rate * 1000.0f;
                    outlet_float(x->x_out_duration, duration_ms);
                    post("mylooper~: recording stopped in DSP thread, loop length = %d samples (%.2f ms)", loop_length, duration_ms);
                }
                break;
                
            case REQUEST_PLAY:
                if (x->play_pos_reset) {
                    play_pos = 0;
                    x->play_pos_reset = 0;
                }
                state = STATE_PLAYING;
                
                // Output loop duration
                if (loop_length > 0 && x->sample_rate > 0) {
                    t_float duration_ms = (t_float)loop_length / x->sample_rate * 1000.0f;
                    outlet_float(x->x_out_duration, duration_ms);
                    post("mylooper~: playback started in DSP thread, duration = %.2f ms", duration_ms);
                }
                break;
                
            case REQUEST_PLAY_ONCE:
                play_pos = 0;
                state = STATE_PLAYING_ONCE;
                post("mylooper~: play_once starting in DSP thread, position reset to %d", play_pos);
                break;
                
            case REQUEST_STOP:
                state = STATE_IDLE;
                
                // Output loop duration
                if (loop_length > 0 && x->sample_rate > 0) {
                    t_float duration_ms = (t_float)loop_length / x->sample_rate * 1000.0f;
                    outlet_float(x->x_out_duration, duration_ms);
                    post("mylooper~: stopped in DSP thread");
                }
                break;
                
            case REQUEST_OVERDUB:
                // Si nous venons du mode enregistrement, nous devons d'abord finaliser la longueur de boucle
                if (x->state == STATE_RECORDING) {
                    // Make sure loop length is a multiple of the buffer size (n)
                    if (loop_length > 0) {
                        int remainder = loop_length % n;
                        if (remainder > 0) {
                            // Round up to the next multiple of n
                            int additional = n - remainder;
                            if ((loop_length + additional) <= buffer_size) {
                                // Zero out the additional samples
                                for (int i = loop_length; i < loop_length + additional; i++) {
                                    buffer[i] = 0.0f;
                                }
                                loop_length += additional;
                                post("mylooper~: adjusted loop length to %d samples to ensure it's a multiple of buffer size %d", loop_length, n);
                            }
                        }
                    }
                    post("mylooper~: switching from recording to overdubbing in DSP thread");
                }
                
                state = STATE_OVERDUBBING;
                
                // Output loop duration
                if (loop_length > 0 && x->sample_rate > 0) {
                    t_float duration_ms = (t_float)loop_length / x->sample_rate * 1000.0f;
                    outlet_float(x->x_out_duration, duration_ms);
                    post("mylooper~: overdubbing started in DSP thread");
                }
                break;
                
            case REQUEST_PAUSE:
                state = STATE_PAUSED;
                post("mylooper~: paused in DSP thread");
                break;
                
            case REQUEST_CLEAR:
                if (buffer) {
                    memset(buffer, 0, buffer_size * sizeof(t_sample));
                }
                loop_length = 0;
                write_pos = 0;
                play_pos = 0;
                state = STATE_IDLE;
                
                // Output zero duration
                outlet_float(x->x_out_duration, 0.0f);
                post("mylooper~: buffer cleared in DSP thread");
                break;
                
            case REQUEST_WAIT_SYNC:
                state = STATE_WAITING_SYNC;
                post("mylooper~: waiting for sync in DSP thread");
                break;
        }
        
        // Clear the request after processing
        x->pending_request = REQUEST_NONE;
        
        // Output state change immediately
        outlet_float(x->x_out_state, (t_float)state);
    }
    
    // Safety check: if buffer not allocated, pass through and return
    if (!buffer) {
        while (n--) {
            *out++ = *in++;
        }
        return (w + 5);
    }
    
    // Process each sample according to current state
    for (int i = 0; i < n; i++) {
        t_sample input = *in++;
        t_sample output = 0.0f;
        
        switch (state) {
            case STATE_IDLE:
                // No signal output in idle state
                output = 0.0f;
                break;
                
            case STATE_RECORDING:
                // Record input to buffer but do not pass through
                if (write_pos < buffer_size) {
                    buffer[write_pos++] = input;
                    // Update loop length as we record
                    loop_length = write_pos;
                } else {
                    // If buffer is full, switch to playing state
                    state = STATE_PLAYING;
                }
                // No direct output during recording
                output = 0.0f;
                break;
                
            case STATE_PLAYING:
                // Play back buffer and don't record input
                if (loop_length > 0) {
                    output = buffer[play_pos++];
                    if (play_pos >= loop_length) {
                        play_pos = 0; // Loop around
                        state = STATE_IDLE; // loop is over
                        // Send sync message when loop wraps (only if master)
                        if (x->master && x->sync_enabled) {
                            outlet_bang(x->x_out_sync);
                            post("mylooper~: sync bang sent (master)");
                        }
                    }
                } else {
                    output = 0.0f; // No recorded material, output silence
                }
                break;
                
            case STATE_PLAYING_ONCE:
                // Play back buffer once and then stop
                if (loop_length > 0 && play_pos < loop_length) {
                    // Play the current sample
                    output = buffer[play_pos];
                    
                    // Check if we're at the beginning of playback
                    if (play_pos == 0) {
                        // Log start of playback
                        post("mylooper~: play_once starting actual audio playback");
                    }
                    
                    // Increment position AFTER reading the sample
                    play_pos++;
                    
                    // Check if we've reached the end AFTER incrementing
                    if (play_pos >= loop_length) {
                        // We've played the entire buffer including the last sample
                        
                        // Reset play position before state transition
                        play_pos = 0;
                        
                        // Set our protection flag BEFORE changing state
                        x->just_finished_playonce = 1;
                        
                        // Critical change: log our position to help debug
                        post("mylooper~: play_once reached end, resetting position to %d", play_pos);
                        
                        // NOW transition to IDLE - but only if we're not in forced play mode
                        state = STATE_IDLE;
                        
                        // Send sync message if master
                        if (x->master && x->sync_enabled) {
                            // First log, then send bang
                            post("mylooper~: one-shot playback finished, sending sync");
                            outlet_bang(x->x_out_sync);
                            post("mylooper~: sync bang sent (master - play_once finished)");
                        } else {
                            post("mylooper~: one-shot playback finished");
                        }
                        
                        // The flag will be reset in the next DSP cycle to ensure stable timing
                    }
                } else {
                    output = 0.0f; // No recorded material or playback done
                }
                break;
                
            case STATE_OVERDUBBING:
                // Playback + record input with overdub level
                if (loop_length > 0) {
                    // Mix existing audio with new input
                    output = buffer[play_pos];
                    buffer[play_pos] = (buffer[play_pos] * feedback) + (input * overdub_level);
                    
                    play_pos++;
                    if (play_pos >= loop_length) {
                        play_pos = 0; // Loop around
                        
                        // Send sync message when loop wraps (only if master)
                        if (x->master && x->sync_enabled) {
                            outlet_bang(x->x_out_sync);
                            post("mylooper~: sync bang sent (master - overdub)");
                        }
                    }
                } else {
                    // If nothing was recorded yet, act as in recording state
                    if (write_pos < buffer_size) {
                        buffer[write_pos++] = input;
                        loop_length = write_pos;
                    }
                    output = input;
                }
                break;
                
            case STATE_PAUSED:
                // Output silence
                output = 0.0f;
                break;
                
            case STATE_WAITING_SYNC:
                // Output existing loop but wait for sync before recording/overdubbing
                if (loop_length > 0) {
                    output = buffer[play_pos++];
                    if (play_pos >= loop_length) {
                        play_pos = 0;
                    }
                } else {
                    // No direct output if nothing recorded yet
                    output = 0.0f;
                }
                break;
        }
        
        *out++ = output;
    }
    
    // Remember previous state
    t_loop_state prev_state = x->state;
    
    // Update instance state
    x->state = state;
    x->write_pos = write_pos;
    x->play_pos = play_pos;
    x->loop_length = loop_length;
    
    // Output duration only when transitioning from RECORDING to another state
    // This avoids continuous output during recording
    if (prev_state == STATE_RECORDING && state != STATE_RECORDING && loop_length > 0) {
        output_loop_duration(x);
    }
    
    // Update elapsed time (for sync calculations)
    x->elapsed_time += (float)n / x->sample_rate;
    
    // Update sync phase if sync is enabled
    if (x->sync_enabled && loop_length > 0) {
        x->sync_phase = (float)play_pos / (float)loop_length;
    }
    
    // Reset the protection flag if we're not going directly into another play_once
    if (x->just_finished_playonce && x->pending_request != REQUEST_PLAY_ONCE) {
        x->just_finished_playonce = 0;
        post("mylooper~: cleared protection flag, ready for next play_once");
    }
    
    return (w + 5);
}

// DSP method
static void mylooper_tilde_dsp(t_mylooper_tilde *x, t_signal **sp)
{
    // Get the current sample rate from Pure Data's audio engine
    x->sample_rate = sp[0]->s_sr;
    
    // Add our perform routine to the DSP chain
    dsp_add(mylooper_tilde_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
}

// Clear the buffer
static void mylooper_tilde_clear(t_mylooper_tilde *x)
{
    x->pending_request = REQUEST_CLEAR;
    post("mylooper~: clear requested, will apply on next DSP cycle");
}

// Start recording
static void mylooper_tilde_record(t_mylooper_tilde *x)
{
    // Instead of changing state directly, set a request flag for the DSP thread
    x->pending_request = REQUEST_RECORD;
    post("mylooper~: recording requested, will start on next DSP cycle");
}

// Stop recording without starting playback
static void mylooper_tilde_stop_recording(t_mylooper_tilde *x)
{
    // Only relevant if we're currently recording
    if (x->state == STATE_RECORDING) {
        x->pending_request = REQUEST_STOP_RECORDING;
        post("mylooper~: stop recording requested, will apply on next DSP cycle");
    }
}

// Start playback
static void mylooper_tilde_play(t_mylooper_tilde *x)
{
    if (x->loop_length > 0) {
        x->pending_request = REQUEST_PLAY;
        x->play_pos_reset = 1;  // Request to reset play position
        post("mylooper~: playback requested, will start on next DSP cycle");
    } else {
        pd_error(x, "mylooper~: nothing to play (record something first)");
    }
}

// Stop any active process and go to idle
static void mylooper_tilde_stop(t_mylooper_tilde *x)
{
    x->pending_request = REQUEST_STOP;
    post("mylooper~: stop requested, will apply on next DSP cycle");
}

// Start overdubbing
static void mylooper_tilde_overdub(t_mylooper_tilde *x)
{
    // Si nous sommes en train d'enregistrer, nous arrêtons l'enregistrement et passons en mode overdub
    if (x->state == STATE_RECORDING) {
        // La boucle a déjà du contenu puisqu'on est en train d'enregistrer
        x->pending_request = REQUEST_OVERDUB;
        post("mylooper~: recording stopped, overdubbing requested, will start on next DSP cycle");
    }
    // Sinon, comportement normal: vérifier qu'une boucle existe
    else if (x->loop_length > 0) {
        x->pending_request = REQUEST_OVERDUB;
        post("mylooper~: overdubbing requested, will start on next DSP cycle");
    } else {
        pd_error(x, "mylooper~: nothing to overdub (record something first)");
    }
}

// Play loop once and then stop
static void mylooper_tilde_play_once(t_mylooper_tilde *x)
{
    if (x->loop_length > 0) {
        // Use the new request system
        x->pending_request = REQUEST_PLAY_ONCE;
        x->play_pos_reset = 1;  // Request to reset play position
        
        // Log that we've requested a play_once
        post("mylooper~: play_once requested - Previous state: %d, will start on next DSP cycle", 
            (int)x->state);
    } else {
        pd_error(x, "mylooper~: nothing to play (record something first)");
    }
}

// Pause playback
static void mylooper_tilde_pause(t_mylooper_tilde *x)
{
    x->pending_request = REQUEST_PAUSE;
    post("mylooper~: pause requested, will apply on next DSP cycle");
}

// Set overdub level
static void mylooper_tilde_overdub_level(t_mylooper_tilde *x, t_floatarg level)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    
    x->overdub_level = level;
    post("mylooper~: overdub level set to %.2f", level);
}

// Set feedback level
static void mylooper_tilde_feedback(t_mylooper_tilde *x, t_floatarg fb)
{
    if (fb < 0.0f) fb = 0.0f;
    if (fb > 1.0f) fb = 1.0f;
    
    x->feedback = fb;
    post("mylooper~: feedback level set to %.2f", fb);
}

// Enable/disable sync mode
static void mylooper_tilde_sync(t_mylooper_tilde *x, t_floatarg enable)
{
    x->sync_enabled = (enable != 0.0f);
    post("mylooper~: sync %s", x->sync_enabled ? "enabled" : "disabled");
}

// Set as master or slave
static void mylooper_tilde_master(t_mylooper_tilde *x, t_floatarg is_master)
{
    x->master = (is_master != 0.0f);
    if (x->master) {
        // Automatically enable sync when setting to master
        x->sync_enabled = 1;
    }
    post("mylooper~: set as %s (sync %s)", x->master ? "master" : "slave", 
         x->sync_enabled ? "enabled" : "disabled");
}

// Set BPM for sync
static void mylooper_tilde_bpm(t_mylooper_tilde *x, t_floatarg bpm)
{
    if (bpm < 20.0f) bpm = 20.0f;
    if (bpm > 300.0f) bpm = 300.0f;
    
    x->bpm = bpm;
    post("mylooper~: BPM set to %.1f", bpm);
}

// Receive sync from another looper or external clock
static void mylooper_tilde_sync_in(t_mylooper_tilde *x)
{
    if (!x->master && x->sync_enabled) {
        if (x->state == STATE_WAITING_SYNC) {
            // ALWAYS reset play position to beginning on sync
            x->play_pos = 0;
            
            // Transition to appropriate state based on context
            if (x->loop_length == 0) {
                update_state(x, STATE_RECORDING);
                post("mylooper~: sync received, started recording");
            } else {
                update_state(x, STATE_PLAYING);
                post("mylooper~: sync received, started playing from beginning");
            }
        } 
        // For play_once looping - also respond to sync when in idle state
        else if (x->state == STATE_IDLE && x->loop_length > 0) {
            // Use the new request system
            x->pending_request = REQUEST_PLAY_ONCE;
            x->play_pos_reset = 1;  // Request to reset play position
            
            // Clear the protection flag
            x->just_finished_playonce = 0;
            
            post("mylooper~: sync received, play_once scheduled for next DSP cycle");
        }
    }
}

// Wait for sync before next state change
static void mylooper_tilde_wait_sync(t_mylooper_tilde *x)
{
    update_state(x, STATE_WAITING_SYNC);
    post("mylooper~: waiting for sync");
}

// Destructor
static void mylooper_tilde_free(t_mylooper_tilde *x)
{
    free_buffer(x);
}

// Constructor
static void *mylooper_tilde_new(void)
{
    t_mylooper_tilde *x = (t_mylooper_tilde *)pd_new(mylooper_tilde_class);
    
    // Initialize buffer and parameters
    x->buffer = NULL;
    x->buffer_size = 0;
    x->loop_length = 0;
    x->write_pos = 0;
    x->play_pos = 0;
    
    // Set default parameter values
    x->state = STATE_IDLE;
    x->overdub_level = 0.8f;
    x->feedback = 0.95f;
    x->sync_enabled = 0;
    x->master = 0;
    x->bpm = 120.0f;
    x->beats_per_loop = 4;
    x->sample_rate = 44100.0f;
    x->elapsed_time = 0.0f;
    x->sync_phase = 0.0f;
    
    // Initialize request flags
    x->pending_request = REQUEST_NONE;
    x->play_pos_reset = 0;
    x->just_finished_playonce = 0;
    
    // Allocate initial buffer (will be reallocated when sample rate is known)
    allocate_buffer(x, 0);
    
    // Create second inlet for messages (accepts any message type)
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, 0, 0);
    
    // Create outlets
    x->x_out_signal = outlet_new(&x->x_obj, &s_signal);
    x->x_out_state = outlet_new(&x->x_obj, &s_float);
    x->x_out_sync = outlet_new(&x->x_obj, &s_bang);
    x->x_out_duration = outlet_new(&x->x_obj, &s_float);
    
    // Send initial state and duration (0 ms)
    outlet_float(x->x_out_state, (t_float)STATE_IDLE);
    outlet_float(x->x_out_duration, 0.0f);
    
    return (void *)x;
}

// Setup function
void mylooper_tilde_setup(void)
{
    mylooper_tilde_class = class_new(gensym("mylooper~"),
                              (t_newmethod)mylooper_tilde_new,
                              (t_method)mylooper_tilde_free,
                              sizeof(t_mylooper_tilde),
                              CLASS_DEFAULT,
                              0);
    
    // Add DSP method
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_dsp,
                    gensym("dsp"), A_CANT, 0);
    
    // Add control methods
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_record,
                    gensym("record"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_stop_recording,
                    gensym("stop_recording"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_play,
                    gensym("play"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_play_once,
                    gensym("play_once"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_stop,
                    gensym("stop"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_overdub,
                    gensym("overdub"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_pause,
                    gensym("pause"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_clear,
                    gensym("clear"), 0);
    
    // Parameter methods
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_overdub_level,
                    gensym("overdub_level"), A_FLOAT, 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_feedback,
                    gensym("feedback"), A_FLOAT, 0);
                    
    // Sync methods
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_sync,
                    gensym("sync"), A_FLOAT, 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_master,
                    gensym("master"), A_FLOAT, 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_bpm,
                    gensym("bpm"), A_FLOAT, 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_sync_in,
                    gensym("sync_in"), 0);
    class_addmethod(mylooper_tilde_class, (t_method)mylooper_tilde_wait_sync,
                    gensym("wait_sync"), 0);
    
    // This is required for signal objects
    CLASS_MAINSIGNALIN(mylooper_tilde_class, t_mylooper_tilde, f);
}
