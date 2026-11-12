// Saadin heating controller (heatctl), see docs/heat-control.md
// Pure logic, no ESP-IDF calls, so it also builds as a host unit test (see test/heatctl_test.c)
#pragma once

#include <stdint.h>

#define HEATCTL_LEVELS_MAX 12
#define HEATCTL_CURVE_MAX 8
#define HEATCTL_DEFROSTS_MAX 16     // Defrost start times kept for the defrost-loop watchdog
#define HEATCTL_OUTDOOR_SMOOTH 1800 // Outdoor temperature smoothing (s)
#define HEATCTL_DT_MAX 60           // Longest step integrated (s)
#define HEATCTL_DEFROST_SPREAD 7200 // The heat a defrost cost is owed over about this long (s)
#define HEATCTL_SCALE_SMOOTH 1800   // Smoothing of the heat the unit gives per watt of the demand table (s)
#define HEATCTL_FAN_LEAVE (-1)      // Fan: leave alone
#define HEATCTL_FAN_MAP (-2)        // Fan: by outdoor temperature, from the state's fan map

typedef struct {    // A demand level, one entry of demand.table
    uint8_t demand; // Demand (%)
    uint8_t quiet;  // Quiet outdoor (QO) with this level, on for all but plain 100
    float watts;    // Electrical watts at this level, times demand.cop for its heat
} heatctl_level_t;

typedef struct {
    int n;
    heatctl_level_t level[HEATCTL_LEVELS_MAX]; // In order of increasing watts
} heatctl_table_t;

typedef struct {
    int n;
    float outdoor[HEATCTL_CURVE_MAX]; // Increasing
    float offset[HEATCTL_CURVE_MAX];
} heatctl_curve_t;

typedef struct {
    int n;
    float outdoor[HEATCTL_CURVE_MAX]; // Increasing
    int8_t fan[HEATCTL_CURVE_MAX];    // 0 auto, 1-5, 6 quiet (as daikin.fan)
} heatctl_fanmap_t;

typedef enum {
    HEATCTL_OFF,      // Disabled, unit off, or not heating: nothing is sent
    HEATCTL_IDLE,     // Heating off (low setpoint)
    HEATCTL_DEMAND,   // Fixed setpoint, PI loop on demand
    HEATCTL_COLD,     // Demand 100, PI loop on setpoint
    HEATCTL_NOSENSOR, // No room reading, plain unit thermostat
} heatctl_state_t;

typedef enum {
    HEATCTL_SUB_NONE,
    HEATCTL_SUB_DEFROST, // Unit is defrosting (DEMAND or COLD)
    HEATCTL_SUB_HOLD,    // Post-defrost hold (DEMAND or COLD)
    HEATCTL_SUB_REST,    // Defrost-loop watchdog rest (IDLE)
} heatctl_sub_t;

typedef struct {
    float target;          // Target room temperature before the curve (auto.t)
    heatctl_table_t table; // Demand table (demand.table)
    heatctl_curve_t curve; // Target offset by outdoor temperature (auto.curve)
    uint32_t stale;        // No fresh room reading for this long goes to NOSENSOR (s)
    uint32_t smooth;       // Room temperature smoothing (s)
    float tmin, tmax;      // Setpoint limits
    // DEMAND
    float demand_set;       // Setpoint
    uint8_t demand_full;    // May use plain 100 (QO off)
    uint8_t demand_start;   // Demand (%) whose heat the integral starts at
    uint32_t demand_rate;   // Minimum time between demand changes (s)
    uint32_t demand_minrun; // Minimum DEMAND run before IDLE (s)
    float cop;              // Heat per electrical watt, roughly: the table as heat where the heat output is not known
    float kp;               // W of heat per °C
    float ti;               // Integral time (s), 0 for none
    // IDLE
    float idle_temp;   // Setpoint
    float idle_over;   // Go IDLE above target + this
    float idle_under;  // Leave IDLE below target - this
    uint32_t idle_min; // Minimum IDLE time (s)
    // COLD
    float cold_enter;   // Enter COLD below this outdoor temperature
    float cold_exit;    // Leave COLD above this outdoor temperature
    uint32_t cold_hold; // For this long (s)
    float cold_set;     // Setpoint on entering COLD
    uint32_t cold_rate; // Minimum time between 0.5 °C setpoint steps (s)
    float cold_kp;      // °C setpoint per °C error
    float cold_ti;      // Integral time (s), 0 for none
    // DEFROST
    float defrost_enter;      // Liquid below this is a defrost
    float defrost_exit;       // Liquid at or above this ends it
    uint32_t defrost_max;     // Safety cap on a defrost (s)
    uint32_t defrost_hold;    // Post-defrost hold (s)
    uint8_t defrost_holdmax;  // Demand (%) at most in the hold, 100 is 100q (never plain 100)
    uint8_t defrost_watchdog; // Defrost-loop watchdog on
    uint8_t defrost_loops;    // More defrosts than this within defrost_window...
    uint32_t defrost_window;  // ...(s)...
    uint32_t defrost_rest;    // ...rest in IDLE for this long (s)
    // Fan per state: HEATCTL_FAN_LEAVE, HEATCTL_FAN_MAP, or 0 auto, 1-5, 6 quiet (as daikin.fan)
    int8_t idle_fan, demand_fan, cold_fan;
    heatctl_fanmap_t idle_fanmap, demand_fanmap, cold_fanmap; // Fan by outdoor temperature (HEATCTL_FAN_MAP)
    uint32_t fan_hold;                                        // A fan from a map is kept this long at least (s)
} heatctl_params_t;

typedef struct {
    uint8_t enabled; // Controller enabled and a target set
    uint8_t power;   // Unit is on
    uint8_t heat;    // Unit is in heat mode
    float room;      // Fresh room temperature, NAN if none
    float outdoor;   // Unit's outdoor temperature, NAN if unknown
    float liquid;    // Unit's liquid temperature, NAN if unknown
    float heatout;   // Estimated heat output (W), NAN if unknown
} heatctl_in_t;

typedef struct {
    heatctl_state_t state;
    heatctl_sub_t sub;
    uint8_t demand;     // Demand to send (%)
    uint8_t quiet;      // Quiet outdoor to send
    float setpoint;     // Setpoint to send
    int8_t fan;         // Fan to send, or HEATCTL_FAN_LEAVE
    float target;       // Effective target (auto.t + curve)
    float curve;        // Curve offset in target
    float heatw;        // Heat wanted (W of heat), NAN when not in use
    float integral;     // Integral: W of heat in DEMAND/IDLE, setpoint °C in COLD
    float owed;         // Heat owed (Wh): heat wanted less heat given so far, negative if over, NAN when not in use
    float give;         // Heat to give now (W): the heat wanted and the making up for defrosts, NAN when not in use
    float scale;        // Heat taken per watt of the demand table (W per W), NAN when not in use
    float room;         // Smoothed room temperature, NAN if none
    float outdoor;      // Smoothed outdoor temperature, NAN if unknown
    uint8_t changed;    // State or sub-state changed in this step
    uint8_t resend_fan; // Defrost just ended, resend the fan
    char reason[100];   // Why it changed
} heatctl_out_t;

typedef struct {
    heatctl_state_t state;
    heatctl_sub_t sub;
    heatctl_state_t resume; // State to resume after the watchdog rest
    uint8_t ticked;         // last is valid
    uint8_t armed;          // Liquid hot since DEMAND/COLD began and since the last defrost, so a drop is a defrost
    uint32_t last;          // Time of last step
    uint32_t since;         // Time the state was entered
    uint32_t sub_since;     // Time the sub-state was entered
    uint8_t room_ok;        // room is valid
    uint8_t room_fresh;     // Last step had a room reading
    uint8_t outdoor_ok;     // outdoor is valid
    uint8_t is_cold;        // Outdoor below cold_enter since cold_since
    uint8_t is_warm;        // Outdoor above cold_exit since warm_since
    uint32_t room_at;       // Time of the last fresh room reading (or start)
    float room;             // Smoothed room
    float outdoor;          // Smoothed outdoor
    uint32_t cold_since;
    uint32_t warm_since;
    float integral;            // DEMAND integral (W of heat)
    float owed;                // DEMAND heat owed (Wh)
    float debt;                // Heat a defrost cost (Wh), not yet in owed
    float scale;               // Heat the unit gives per watt of the demand table, smoothed, 0 if not set yet
    float cold_integral;       // COLD integral (°C)
    float heatw;               // Last heat wanted (W of heat)
    int level;                 // Current level (DEMAND)
    uint32_t level_at;         // Time of the last demand change
    float setpoint;            // Current setpoint (COLD)
    uint32_t setpoint_at;      // Time of the last setpoint change (COLD)
    heatctl_state_t fan_state; // State the fan from a map was set in, HEATCTL_OFF if none
    int8_t fan;                // Fan from a map
    uint32_t fan_at;           // Time it was set
    uint8_t defrosts;          // Entries in defrost_at
    uint32_t defrost_at[HEATCTL_DEFROSTS_MAX];
} heatctl_t;

// Parse a demand table "demand:watts,..." ("100q" is 100 with QO). Empty means def, the default (demand.table's
// default in settings.def). Returns NULL if OK, else an error, and the table is then def.
const char *heatctl_parse_table(heatctl_table_t *t, const char *s, const char *def);

// Parse a target curve "outdoor:offset,...". Empty means no offset. Returns NULL if OK, else an error, and the curve
// is then empty.
const char *heatctl_parse_curve(heatctl_curve_t *c, const char *s);

// Target offset at this outdoor temperature: linear between points, flat beyond the ends, 0 if empty or unknown.
float heatctl_curve_offset(const heatctl_curve_t *c, float outdoor);

// Parse a fan map "outdoor:fan,...", fan being A (auto), 1 to 5 or Q (quiet). Returns NULL if OK, else an error, and
// the map is then empty.
const char *heatctl_parse_fanmap(heatctl_fanmap_t *m, const char *s);

// Fan at this outdoor temperature: that of the highest point at or below it, or of the first point below them all.
// HEATCTL_FAN_LEAVE if empty or unknown.
int8_t heatctl_fanmap_fan(const heatctl_fanmap_t *m, float outdoor);

// One control step. now is in seconds and must not go backwards. Call it about once a second.
heatctl_out_t heatctl_step(heatctl_t *s, const heatctl_params_t *p, const heatctl_in_t *in, uint32_t now);

const char *heatctl_state_name(heatctl_state_t state);
const char *heatctl_sub_name(heatctl_sub_t sub);

// Room temperature after a check of the BLE sensor. reading is its temperature, or NAN if it has none. *from_ble says
// whether env holds a BLE reading. Once the sensor has none, its last reading becomes NAN (unknown), while a value
// from elsewhere (MQTT) is kept.
float heatctl_ble_env(float env, uint8_t *from_ble, float reading);
