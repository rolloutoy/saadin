// Saadin heating log (heatlog): one sample a minute for the graph page, see Manuals/Heating.md
// Pure logic, no ESP-IDF calls, so it also builds as a host unit test (see test/heatlog_test.c)
#pragma once

#include <stdint.h>

#define HEATLOG_INTERVAL 60    // Seconds per sample
#define HEATLOG_NONE INT16_MIN // Value not known

typedef struct {     // Inputs, about once a second, NAN if not known
    float room;      // Room temperature (°C), mean over the sample
    float outdoor;   // Outdoor temperature (°C), mean
    float heatw;     // Heat wanted (W), mean
    float power;     // Power consumption (W), mean
    float target;    // Target, auto.t (°C), last in the sample
    float effective; // Effective target, with the curve (°C), last
    float setpoint;  // Unit setpoint (°C), last
    float demand;    // Unit demand (%), last
    uint8_t state;   // heatctl_state_t, last
    uint8_t sub;     // heatctl_sub_t, last, but a defrost anywhere in the sample wins
} heatlog_in_t;

typedef struct { // One sample, temperatures in 0.01 °C, HEATLOG_NONE if not known
    uint32_t t;  // Time the sample ended (s, as given to heatlog_add)
    int16_t room;
    int16_t outdoor;
    int16_t heatw; // W
    int16_t power; // W
    int16_t target;
    int16_t effective;
    int16_t setpoint;
    int16_t demand; // %
    uint8_t state;
    uint8_t sub;
} heatlog_sample_t;

#define HEATLOG_MEANS 4 // room, outdoor, heatw, power

typedef struct {
    heatlog_sample_t *sample; // Ring of n samples
    uint32_t n;               // Size of the ring
    uint32_t count;           // Samples held
    uint32_t next;            // Where the next sample goes
    uint32_t start;           // When the sample being built started, 0 for none yet
    float sum[HEATLOG_MEANS]; // Sample being built
    uint32_t num[HEATLOG_MEANS];
    heatlog_in_t last;
    uint8_t defrost; // A defrost in the sample being built
} heatlog_t;

// Start an empty log in buf, n samples. buf may be NULL (n 0), then nothing is logged.
void heatlog_init(heatlog_t *l, heatlog_sample_t *buf, uint32_t n);

// Add the inputs at now (s, must not go backwards, not 0). Call it about once a second. A sample is kept every
// HEATLOG_INTERVAL seconds.
void heatlog_add(heatlog_t *l, const heatlog_in_t *in, uint32_t now);

// Copy up to max samples, oldest first, the newest if there are more. Returns the number copied.
uint32_t heatlog_get(const heatlog_t *l, heatlog_sample_t *out, uint32_t max);
