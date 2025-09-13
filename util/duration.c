// This file contains utility functions for duration-related calculations for myLooper~
// It is currently not used directly but provided for future expansion

#include "m_pd.h"
#include <math.h>

// Convert samples to milliseconds
float samples_to_ms(int num_samples, float sample_rate) {
    return (float)num_samples / sample_rate * 1000.0f;
}

// Convert milliseconds to samples
int ms_to_samples(float ms, float sample_rate) {
    return (int)(ms * sample_rate / 1000.0f);
}

// Convert beats to milliseconds
float beats_to_ms(float beats, float bpm) {
    float beat_duration_ms = 60000.0f / bpm;  // One beat in milliseconds
    return beats * beat_duration_ms;
}

// Convert milliseconds to beats
float ms_to_beats(float ms, float bpm) {
    float beat_duration_ms = 60000.0f / bpm;  // One beat in milliseconds
    return ms / beat_duration_ms;
}