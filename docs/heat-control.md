# Heating control (heatctl)

Design for Säädin's heating controller. It replaces Faikout's local auto mode completely. For how to use it, see [Manuals/Heating.md](../Manuals/Heating.md). Background, measurements and forum findings are in `saadin-ideas.md` sections 1.1–1.5 (private notes, not in this repo); this document is the agreed design.

Terms: **QO** is quiet outdoor. **PI** is a proportional-integral control loop. **Demand** is the unit's power limit, in %.

Heating only. In cool, dry or fan mode, or with the unit off, the controller does nothing and the unit runs on its own settings. The controller never powers the unit on or off.

## States

```
              room > target + idle.over              outdoor < cold.enter (held cold.hold)
   IDLE  <──────────────────────────────  DEMAND  ──────────────────────────────>  COLD
         ──────────────────────────────>          <──────────────────────────────
              room < target − idle.under             outdoor > cold.exit (held cold.hold)
              (after idle.min)               │                                    │
                                     DEMAND.DEFROST                         COLD.DEFROST

   NOSENSOR: no fresh room reading for 30 min, from any state except OFF
   OFF: controller disabled, unit off, or unit not in heat mode
```

### OFF
Controller disabled (`auto.e` off or `auto.t` 0), unit off, or unit not in heat mode. Nothing is sent while OFF.

On the way into OFF, for whichever reason, the controller leaves nothing behind: demand goes back to 100% and QO off, and the setpoint goes to `stop.temp` (default 22 °C), clamped to `t.min`…`t.max`. The setpoint is only replaced if it is still the one the controller sent, so a setpoint set by hand at the same time (e.g. with a switch to cool) is kept.

When the controller starts (enabled, unit on in heat) it starts **fresh** in DEMAND with the integral at the watts of the `demand.start` level (default 60%). No NVS persistence.

### IDLE ("heating off")
- Sends setpoint `idle.temp` (default 16 °C) so the unit stops heating. Demand stays at the lowest level with QO on, so the restart is gentle.
- Entered only from DEMAND, when room > target + `idle.over` (default 0.5 °C) **and** DEMAND has run at least `demand.minrun` (default 30 min; short runs stop defrosts working, Faikout #930).
- Exits to DEMAND when room < target − `idle.under` (default 0.0 °C) **and** IDLE has lasted at least `idle.min` (default 30 min).
- On exit the PI integral starts at the watts of the lowest level.

### DEMAND
- Sends the fixed setpoint `demand.set` (default 30 °C, the highest heat setpoint many units accept).
- PI loop on room temperature error. Output is **heat wanted in electrical watts**. No D term.
- The demand table (below) maps watts to a demand level (a demand % with or without QO): the highest level whose watts ≤ wanted.
- **Quiet outdoor (QO) is on for every level except plain 100%.** That includes the `100q` level, IDLE and the DEFROST sub-state; it calms defrosts and the ramp-up after them.
- `demand.full` (bit, default on): when on, DEMAND may use plain 100% (QO off). When off, DEMAND tops out at 100 + QO.
- Demand changes at most once per `demand.rate` (default 600 s), but one change may jump several levels.
- Anti-windup: the integral stops growing when at the top or bottom level, during DEFROST and the post-defrost hold, and while the room reading is stale.
- Gains: `pid.kp` (W per °C, default 300), `pid.ti` (minutes, default 90). On the heat-model PoC's two-node house these hold ±0.2 K from −12 to +10 °C outside.

### COLD
- Sends demand 100 with QO **always off**.
- A second PI loop drives the **setpoint** from room temperature error. It starts at `cold.set` (default 23 °C), is clamped to `t.min`…`t.max`, rounded to 0.5 °C, and moves at most 0.5 °C per `cold.rate` (default 1800 s); big setpoint jumps are reported to cause defrost loops. Gains `cold.kp` (°C setpoint per °C error, default 2.0), `cold.ti` (minutes, default 120), not yet tested on hardware.
- No IDLE from COLD: on overshoot the setpoint PI pulls the setpoint down and the unit thermostats off by itself.
- Entered from DEMAND when the smoothed outdoor temperature stays below `cold.enter` (default −15 °C) for `cold.hold` (default 60 min). Exits to DEMAND when it stays above `cold.exit` (default −10 °C) for `cold.hold`.
- On exit to DEMAND the integral starts at the watts of the 95% level (the house has just needed a lot of heat).
- Unknown outdoor temperature never enters COLD.

### DEFROST sub-state (of DEMAND and of COLD)
- Enter when `liquid` < `defrost.enter` (default 5 °C). Exit when `liquid` ≥ `defrost.exit` (default 25 °C), or after `defrost.max` (default 30 min) as a safety cap.
- A defrost pulls the coil to about −25…−30 °C. A coil that only stopped heating (the unit thermostats off, or the compressor restarts) drifts down towards room temperature, so 5 °C keeps clear of both.
- A drop only counts once `liquid` has reached `defrost.exit` since DEMAND or COLD was entered. A coil that cooled in IDLE, NOSENSOR or the watchdog rest is not a defrost when heating starts again.
- Holds the current outputs, freezes the integral. No IDLE/COLD transitions are evaluated during defrost. Unknown `liquid` never enters defrost.
- Start with `liquid` only. Log `fanrpm`, `comp` and power at entry and exit so the detection can be refined later.
- **Post-defrost hold** (after leaving DEFROST, `defrost.hold`, default 10 min, 0 for no hold): demand capped at 95% (no plain 100, no 100q), and demand may go down but not up. In COLD the setpoint may go down but not up. The integral keeps running, so it learns the heat the defrost cost. It carries over between DEMAND and COLD without restarting, so it lasts `defrost.hold` in all.
- **Fan resend:** after each defrost, resend the configured fan for the current state (the unit doesn't restore a forced fan speed).
- **Defrost-loop watchdog** (`defrost.watchdog`, default off): more than `defrost.loops` (default 3) defrosts within `defrost.window` (default 90 min) → go to IDLE for `defrost.rest` (default 15 min), then resume DEMAND/COLD. Logged and sent on `info/<host>/heatctl`. Breaks the "death loop" users report at −5…−12 °C.

### NOSENSOR
- No fresh room reading for 30 min (`auto.stale`, default 1800 s). Before that the controller freezes the integral and holds its outputs.
- Falls back to the unit's own room sensor: setpoint = target (after curve), demand 100, QO off, i.e. plain unit thermostat.
- Returns to DEMAND (fresh integral as at start) when a reading returns.

## Target and room temperature

- **Target** = `auto.t` + offset from `auto.curve` at the smoothed outdoor temperature.
- `auto.curve` is a string of `outdoor:offset` points, e.g. `"-25:1.0,-10:0.5,0:0"`. Linear between points, flat beyond the ends, empty = no offset. It's an offset, so setting the target from web / HA / MQTT keeps working.
- **Outdoor** temperature is the unit's own reading, smoothed (e.g. 30 min EMA) and frozen during DEFROST (the unit's outdoor reading jumps about +4 °C during defrost).
- **Room** temperature: BLE sensor (`auto.b`) if set and fresh, otherwise MQTT (`auto.topic`/`auto.payload`, or `env` in the `control` command). Smoothed over `auto.smooth` (default 0, none: a sensor with 0.01 °C steps barely flickers, and smoothing only adds lag; set e.g. 300 s for a sensor with 0.1 °C steps). The unit's own sensor is never used as the room reading for control (it reads 3–7 K high against the fixed setpoint); it's only used via NOSENSOR.

## Demand table

`demand.table`, string, default:

```
40:270,50:340,60:400,70:465,75:500,80:530,85:570,90:605,95:640,100q:1510,100:2470
```

`demand:watts` per demand level; `100q` is 100% with QO (QO is on for every level except `100` anyway). Up to 95% the defaults follow about 6.7 W per %, from the `/graph` log on 2026-11-10 (50/60/70% measured at 353/407/460 W at −1…−2 °C outdoor) and forum data for the 4 kW Perfera Cold Region (40% ≈ 270, 70% ≈ 470, 95% ≈ 640 W). 75% and 90% are there to keep the steps even. `100q` and `100` come from the step test in `saadin-ideas.md` 1.3. Levels that add nothing are left out. The integral absorbs table error, so only the shape matters. Invalid table → fall back to the default and log it.

## Fan

Per-state fan settings `idle.fan`, `demand.fan`, `cold.fan`: leave alone, auto, 1–5, or quiet. Applied on entering the state and resent after each defrost. "Leave alone" never touches the fan. A fan the unit doesn't have (2 or 4 on a 3-speed unit, auto or quiet on a unit with neither) counts as "leave alone".

## Outside changes

If the unit reports a demand, setpoint, QO or fan that differs from what the controller wants (IR remote, Onecta, the unit silently reverting demand to 100), the controller resends its values, at most once a minute per value, and logs that it overrode an outside change. Econo is forced off while the controller runs (Econo and demand share `D7`).

## Removed from Faikout's auto mode

- Timed on/off (`auto.0`, `auto.1`), auto power on/off by temperature (`auto.p`, `auto.ptemp`), heat/cool switching (`lockmode`), auto fan (`auto.fmax`, `no.fanauto`), the Faikout BLE remote (all of it; BLE is only for room sensors), the target range of the MQTT `control` command (its `env` stays).
- Settings: `thermostat`, `temp.track`, `temp.adjust`, `temp.noflap`, `heat.over`/`back`, `cool.over`/`back`, `switchtemp`, `pushtemp`, `t.predicts`, `t.predictt`, `t.sample`, `auto.lcontrol`, `t.coolmin`, `t.heatmax` and anything else only the old loop used.
- Kept: `auto.e` (now the controller's on switch), `auto.t`, `auto.b`, `auto.topic`, `auto.payload`, `t.min`, `t.max`, `t.control`, `frosttemp` (separate safety net), `min.outside`.

## Visibility

- **Log:** every state or sub-state change with its reason, e.g. `heatctl: DEMAND -> IDLE (room 21.6 > target 21.0+0.5)`; a status line every 10 min.
- **MQTT:** `info/<host>/heatctl` on every transition (state, sub-state, reason, inputs). Status JSON gains `ctl` (state), `ctl_sub`, `ctl_target`, `ctl_curve`, `ctl_heatw`, `ctl_i`.
- **Web:** the old auto section is replaced by an enable switch, a target slider and a status line such as "Demand 65% · target 21.3 (curve +0.30) · heat 410 W" or "Cold · setpoint 24.5 · ❄ defrost". Manual temperature, demand and QO controls are greyed out while the controller runs.
- **Home Assistant:** an enum sensor "Control state"; `hvac_action` reports `defrosting` / `idle` / `heating`; the climate target sets `auto.t`. Tuning entities: number entities for `auto.t`, `pid.kp`, `pid.ti`; sensors for heat wanted (W), integral and effective target.

## Code layout

- `ESP/main/heatctl.c` / `heatctl.h`: plain C, `float`, no ESP-IDF includes. `heatctl_step(&state, &params, &in, now)` → outputs (state, sub-state, demand, QO, setpoint, fan, effective target, heat wanted W, integral, transition reason). Also parses the table and the curve. Runs as a host unit test.
- `ESP/main/Faikout.c`: the old local auto block is removed. Each control tick gathers inputs, calls `heatctl_step`, applies outputs with the existing `daikin_set_*` setters (resend works because the S21 reports overwrite `daikin.*` when the unit disagrees). No S21 protocol logic changes.
- Settings in `ESP/main/settings.def` only.

## Tests

- `ESP/main/test/heatctl_test.c`, run by `make -C ESP test`: every transition with hysteresis and minimum times; defrost enter / exit / cap; post-defrost hold; watchdog; stale sensor and NOSENSOR; integral start values after each transition; anti-windup; rate limits; QO per level and `demand.full`; table and curve parsing including bad input.
- A small first-order room model inside the test to check the loop settles without hunting between DEMAND and IDLE.
- Firmware builds clean with `make s3` and `make pico`. Hardware testing is done by a human.
