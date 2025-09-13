#ifndef DURATION_H
#define DURATION_H

// Convert samples to milliseconds
float samples_to_ms(int num_samples, float sample_rate);

// Convert milliseconds to samples
int ms_to_samples(float ms, float sample_rate);

// Convert beats to milliseconds
float beats_to_ms(float beats, float bpm);

// Convert milliseconds to beats
float ms_to_beats(float ms, float bpm);

#endif // DURATION_H