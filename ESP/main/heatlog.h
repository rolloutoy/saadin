// Saadin heating log (heatlog): one sample every 30 s for the graph page, see Manuals/Heating.md
// Pure logic, no ESP-IDF calls, so it also builds as a host unit test (see test/heatlog_test.c)
#pragma once

#include <stdint.h>

#define HEATLOG_INTERVAL 30    // Seconds per sample
#define HEATLOG_NONE INT16_MIN // Value not known
#define HEATLOG_NONE8 255      // fanset or quiet not known

typedef struct {     // Inputs, about once a second, NAN if not known
    float room;      // Room temperature, raw sensor reading (°C), mean over the sample
    float outdoor;   // Outdoor temperature (°C), mean
    float heatw;     // Heat wanted (W), mean
    float power;     // Power consumption (W), mean
    float smoothed;  // Room temperature as the controller sees it, smoothed (°C), mean
    float liquid;    // Liquid temperature (°C), mean
    float comp;      // Compressor frequency (Hz), mean
    float fan;       // Indoor fan speed as the unit reports it (rpm), mean
    float intake;    // Unit's own room (intake air) temperature (°C), mean
    float heatout;   // Estimated heat output (W), mean, see heatlog_heatout
    float target;    // Target, auto.t (°C), last in the sample
    float effective; // Effective target, with the curve (°C), last
    float setpoint;  // Unit setpoint (°C), last
    float demand;    // Unit demand (%), last
    float integral;  // PI integral, last: W, or setpoint (°C) in COLD
    float kp;        // PI gain in use, last: W per °C, or °C per °C in COLD
    float ti;        // PI integral time in use (min), last
    uint8_t state;   // heatctl_state_t, last
    uint8_t sub;     // heatctl_sub_t, last, but a defrost anywhere in the sample wins
    uint8_t fanset;  // Fan setting, last: 0 auto, 1-5, 6 quiet (as daikin.fan), HEATLOG_NONE8 if not known
    uint8_t quiet;   // Quiet outdoor, last: 0 or 1, HEATLOG_NONE8 if not known
} heatlog_in_t;

typedef struct { // One sample, temperatures in 0.01 °C, HEATLOG_NONE if not known
    uint32_t t;  // Time the sample ended (s, as given to heatlog_add)
    int16_t room;
    int16_t outdoor;
    int16_t heatw; // W
    int16_t power; // W
    int16_t smoothed;
    int16_t liquid;
    int16_t comp; // Hz
    int16_t fan;  // rpm
    int16_t intake;
    int16_t heatout; // W
    int16_t target;
    int16_t effective;
    int16_t setpoint;
    int16_t demand;   // %
    int16_t integral; // W, or 0.01 °C in COLD
    int16_t kp;       // W per °C, or 0.01 °C per °C in COLD
    int16_t ti;       // min
    uint8_t state;
    uint8_t sub;
    uint8_t fanset;
    uint8_t quiet;
} heatlog_sample_t;

#define HEATLOG_MEANS 10 // room, outdoor, heatw, power, smoothed, liquid, comp, fan, intake, heatout

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

// Estimated heat output (W): the air flow (m3/h) times the temperature rise from intake to out (°C) times the heat
// capacity of air. NAN if any is not known.
float heatlog_heatout(float flow, float out, float intake);

// Copy up to max samples, oldest first, the newest if there are more. Returns the number copied.
uint32_t heatlog_get(const heatlog_t *l, heatlog_sample_t *out, uint32_t max);
