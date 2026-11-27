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

When the controller starts (enabled, unit on in heat) it starts **fresh** in DEMAND with the integral at the heat of the `demand.start` level (default 60%), or in COLD if the outdoor temperature is already below `cold.enter`, except for the first start after a restart, which may resume (below).

### After a restart

After each step the controller writes a small block to RTC memory (`RTC_NOINIT_ATTR` in `Faikout.c`): plain RAM that keeps its contents over a software restart, a panic or a watchdog reset, but not a power cut. No flash or NVS is written. The first start after a boot resumes from it instead of starting fresh.

- **Kept:** the state (DEMAND, COLD or IDLE, and the defrost-loop rest of IDLE with the state to resume after it); in DEMAND the integral, the demand level in use (as demand % and QO, not a table index, so a changed table cannot misplace it), the heat deficit and the defrost debt; in IDLE the integral that DEMAND will carry on with; in COLD the setpoint and the room average; and the learnt heat per watt (`scale`).
- **Not kept:** every timer (`demand.rate`, `demand.minrun`, `idle.min`, `cold.hold` tracking, the post-defrost hold, defrost history, `fan.hold`) and every sub-state but the rest. After a resume they start as on entering the state: a resumed IDLE waits `idle.min` again, a resumed rest lasts `defrost.rest` again, a resumed COLD waits `cold.dwell` before its next setpoint step and needs `cold.hold` above `cold.exit` before it leaves.
- **Not written:** OFF and NOSENSOR leave nothing kept, so a restart after the controller was switched off, or the unit turned off or out of heat, starts fresh, as does a restart in NOSENSOR (leaving NOSENSOR starts fresh anyway).
- **Pending:** after a boot the controller is OFF until the unit's state is known. Until the kept block has been tried (at the first start) it is not written over, so a second restart in those seconds keeps it. It is given up, and written over from then on, if the controller is switched off (`auto.e` off) or the block passes 30 minutes old first.
- **Checked:** a magic number, a version (`HEATCTL_KEEP_VERSION`, bumped whenever a field changes) and a CRC-32 over the block; all values finite; the integral, the setpoint and the scale within the limits the controller clamps them to now; the level in the current `demand.table`; and an age of at most 30 minutes (`HEATCTL_KEEP_AGE`), on the RTC timer (`esp_rtc_get_time_us()`), which runs on over a restart and needs no clock set from the network. If any check fails the start is fresh, and its reason says why, e.g. `start, not resumed: too old`.
- A resume is logged and sent on `info/<host>/heatctl` like any change of state, e.g. `OFF -> DEMAND (resumed after restart, demand 70 QO, integral 1450 W)`. The web status line says "resumed after restart" for 10 minutes.

### IDLE ("heating off")
- Sends setpoint `idle.temp` (default 16 °C) so the unit stops heating. Demand stays at the lowest level with QO on, so the restart is gentle.
- Entered only from DEMAND at the lowest demand level, when room > target + `idle.over` (default 0.5 °C) **and** DEMAND has run at least `demand.minrun` (default 30 min; short runs stop defrosts working, Faikout #930). Above the lowest level an overshoot only steps demand down, so a short overshoot in cold weather does not turn heating off.
- Exits to DEMAND when room < target − `idle.under` (default 0.0 °C) **and** IDLE has lasted at least `idle.min` (default 30 min).
- On exit DEMAND carries on with the integral it had when heating went off, with no heat deficit or defrost debt. Heating mostly goes off on heat from elsewhere (an oven, a sauna, guests), and once that is gone the house needs what it needed before; started at the heat of the lowest level, the integral took hours to climb back while the room sat under the target. In mild weather, where even the lowest level is too much, the integral is already low when heating goes off. The level is the highest whose heat is at or below the heat request, by the learnt heat per watt (`scale`), which is what that integral is in.

### DEMAND
- Sends the fixed setpoint `demand.set` (default 30 °C, the highest heat setpoint many units accept).
- PI loop on room temperature error. Output is the **heat request**, in watts of heat. No D term.
- **Heat target:** the heat request plus the making up for defrosts (below). It is what the unit is to give now.
- **Heat deficit:** every second the controller adds the heat target less the heat the unit gave (the estimated heat output, see `fan.flow`) to the heat deficit (Wh, negative for a surplus). Demand steps between the levels around the heat target so that the deficit stays near nothing. The mean heat given is then the heat request, whatever the gap between two levels (95% → 100q → 100 are about 2200 → 3900 → 6800 W of heat), the COP or the fan speed.
  - A step up is due once the deficit is half of what the next level would give over the heat target in `demand.rate`, the shortest time demand may stay there; from then on the step leaves less of a deficit or surplus than staying does. A step down is the same the other way.
  - Demand only steps up from a level that itself gives less than the heat target, and down from one that gives more. Heat still on its way after a step (the unit takes minutes to ramp up) then does not bring on the next step.
  - One step may go over several levels: it goes on while the next level is still short of the heat target (or still over it).
  - The heat of a level is its watts in the demand table times the heat the unit has given per watt of the table of late (smoothed over 30 minutes, kept within half to twice `demand.cop`). So the table gives the shape and the unit the scale.
  - **Defrosts:** heat not given during a defrost and the hold after it is kept apart and added to the heat target over the next two hours or so. More defrosts then mean a higher heat target between them, without a burst after each one.
  - At the top level the deficit is no more than that level can make up, and at the bottom level the surplus no more than that level can work off, so the deficit does not wind up.
  - **No heat output estimate** (`fan.flow` blank, or the unit does not report what it needs): the heat given is taken as the table's watts times `demand.cop`, and nothing in a defrost.
  - On entering DEMAND the level comes from the table times `demand.cop`, and the deficit starts at nothing.
- `demand.cop` (default 3.0): heat per watt of the demand table, roughly. It turns the table into heat for the integral's start values and limits, and is where the scale above starts.
- **Quiet outdoor (QO) is on for every level except plain 100%.** That includes the `100q` level, IDLE and the DEFROST sub-state; it calms defrosts and the ramp-up after them.
- `demand.full` (bit, default on): when on, DEMAND may use plain 100% (QO off). When off, DEMAND tops out at 100 + QO.
- Demand changes at most once per `demand.rate` (default 600 s), but one change may jump several levels.
- Anti-windup: the integral stops growing when the top level gives no more than the heat request (and stops falling when the bottom level gives no less), during DEFROST, and while the room reading is stale.
- Gains: `pid.kp` (W of heat per °C, default 900), `pid.ti` (minutes, default 90). On the heat-model PoC's two-node house these hold ±0.2 K from −12 to +10 °C outside without defrosts.

### COLD
- Sends demand 100, with QO on while the smoothed outdoor temperature is at or above `cold.quiet` (default −5 °C) and off below it; back on at `cold.quiet` + 0.5 °C. Gentler running at mild temperatures, where defrosts are most frequent; all the unit has in the cold. The outdoor reading is frozen through a defrost and its hold, so QO does not change in them. Unknown outdoor: off. `cold.quiet` at or above `cold.exit` keeps QO off in COLD.
- Target control: no demand limit, the unit's own thermostat runs it, and the controller moves the **setpoint**. Below about +2 °C the unit defrosts often, and demand control copes badly with that; the unit on its own setpoint does not.
- The unit holds its intake sensor at about setpoint + 2.5 °C whenever it is above its lowest compressor speed (2 h means within 0.1 °C in the logs of 2026-11). The room runs below the intake by a gap that grows with the heat given, about 0.5 °C per kW, so the room is about setpoint + 0.5…1 °C. A setpoint step reaches the room about half within half an hour and the rest over many hours, as the house's mass warms. At its lowest speed the unit cannot cut heat further, and a lower setpoint only lets the room coast down, about 0.2 °C/h.
- So the setpoint steps slowly, on an average. The room (after `auto.smooth`) is averaged with an exponential moving average of time `cold.avg` (default 180 min, 0 for none). The defrost dips stay in: they are part of the room the house has. A single defrost moves a 3 h average by 0.05 °C typically and 0.23 °C at most; a weighted moving average of the same span weighs the newest samples twice as much, and a defrost twice as much with them.
- When the average is above target + `cold.band` (default 0.4 °C) the setpoint goes down 0.5 °C, below target − `cold.band` up 0.5 °C. Steps are at least `cold.dwell` (default 180 min) apart, and the first comes `cold.dwell` after entering COLD at the soonest. The setpoint starts at `cold.set` (default 23 °C), rounded to 0.5 °C. Big setpoint jumps are reported to cause defrost loops.
- The setpoint stays within `cold.min`…`cold.max` (defaults 16 and 32 °C, whole degrees), and within `t.min`…`t.max` as every setpoint does. `cold.max` is for the unit that gives all it has and still leaves the room under the target: without it the setpoint climbs on to `t.max`, which heats no more, and once the weather eases it has all that way to come back down, 0.5 °C per `cold.dwell`, with the room over the target meanwhile. A few degrees over the target is enough. `cold.min` does the same for the way down. `cold.set` outside the limits starts at the nearer one, and a limit moved past the setpoint moves the setpoint at once, the next step then `cold.dwell` later. With `cold.min` above `cold.max`, `cold.max` holds.
- No step while the reading is stale or during a defrost; in the post-defrost hold only down. The average keeps running through both while there is a fresh reading.
- No IDLE from COLD: on overshoot the setpoint comes down and the unit thermostats off by itself.
- Entered from DEMAND when the smoothed outdoor temperature stays below `cold.enter` (default +2 °C) for `cold.hold` (default 60 min). A fresh start below `cold.enter` enters COLD at once, with no DEMAND to hold against; so does the first outdoor reading below it, if the start had none. Exits to DEMAND when it stays above `cold.exit` (default +5 °C) for `cold.hold`.
- On exit to DEMAND the integral starts at the heat of the 95% level (the house has just needed a lot of heat).
- Unknown outdoor temperature never enters COLD.

### DEFROST sub-state (of DEMAND and of COLD)
- Enter when `liquid` < `defrost.enter` (default 5 °C). Exit when `liquid` ≥ `defrost.exit` (default 25 °C), or after `defrost.max` (default 30 min) as a safety cap.
- A defrost pulls the coil to about −25…−30 °C. A coil that only stopped heating (the unit thermostats off, or the compressor restarts) drifts down towards room temperature, so 5 °C keeps clear of both.
- A drop only counts once `liquid` has reached `defrost.exit` since DEMAND or COLD was entered. A coil that cooled in IDLE, NOSENSOR or the watchdog rest is not a defrost when heating starts again.
- Holds the current outputs, freezes the integral. No IDLE/COLD transitions are evaluated during defrost. Unknown `liquid` never enters defrost.
- Start with `liquid` only. Log `fanrpm`, `comp` and power at entry and exit so the detection can be refined later.
- **Post-defrost hold** (after leaving DEFROST, `defrost.hold`, default 10 min, 0 for no hold): demand capped at `defrost.holdmax` (default 100, which is 100q; never plain 100, since quiet outdoor alone damps the surge after a defrost), and demand may go down but not up. In COLD the setpoint may go down but not up. The integral keeps running, so it learns the heat the defrost cost. It carries over between DEMAND and COLD without restarting, so it lasts `defrost.hold` in all.
- **Fan resend:** after each defrost, resend the configured fan for the current state (the unit doesn't restore a forced fan speed).
- **Defrost-loop watchdog** (`defrost.watchdog`, default off): more than `defrost.loops` (default 3) defrosts within `defrost.window` (default 90 min) → go to IDLE for `defrost.rest` (default 15 min) with the fan on auto (whatever `idle.fan` is), then resume DEMAND/COLD, DEMAND with the integral it had as after any IDLE. Logged and sent on `info/<host>/heatctl`. Breaks the "death loop" users report at −5…−12 °C.

### NOSENSOR
- No fresh room reading for 30 min (`auto.stale`, default 1800 s). Before that the controller freezes the integral and holds its outputs.
- Falls back to the unit's own room sensor: setpoint = target (after curve), demand 100, QO off, i.e. plain unit thermostat.
- Returns to DEMAND (fresh integral as at start) when a reading returns.

## Target and room temperature

- **Target** = `auto.t` + offset from `auto.curve` at the smoothed outdoor temperature.
- `auto.curve` is a string of `outdoor:offset` points, e.g. `"-25:1.0,-10:0.5,0:0"`. Linear between points, flat beyond the ends, empty = no offset. It's an offset, so setting the target from web / HA / MQTT keeps working.
- **Outdoor** temperature is the unit's own reading, smoothed (e.g. 30 min EMA) and frozen during DEFROST and the post-defrost hold (the unit's outdoor reading jumps about +4 °C during defrost, and spikes again right after it).
- **Room** temperature: BLE sensor (`auto.b`) if set and fresh, otherwise MQTT (`auto.topic`/`auto.payload`, or `env` in the `control` command). Smoothed over `auto.smooth` (default 0, none: a sensor with 0.01 °C steps barely flickers, and smoothing only adds lag; set e.g. 300 s for a sensor with 0.1 °C steps). The unit's own sensor is never used as the room reading for control (it reads 3–7 K high against the fixed setpoint); it's only used via NOSENSOR.

## Demand table

`demand.table`, string, default:

```
40:270,50:340,60:400,70:465,75:500,80:530,85:570,90:605,95:640,100q:1510,100:2470
```

`demand:watts` per demand level; `100q` is 100% with QO (QO is on for every level except `100` anyway). Up to 95% the defaults follow about 6.7 W per %, from the `/graph` log on 2026-11-10 (50/60/70% measured at 353/407/460 W at −1…−2 °C outdoor) and forum data for the 4 kW Perfera Cold Region (40% ≈ 270, 70% ≈ 470, 95% ≈ 640 W). 75% and 90% are there to keep the steps even. `100q` and `100` come from the step test in `saadin-ideas.md` 1.3. Levels that add nothing are left out. The watts are electrical; `demand.cop` and the heat the unit gives (see DEMAND) turn them into heat, so only the shape matters. Invalid table → fall back to the default and log it.

## Fan

Per-state fan settings `idle.fan`, `demand.fan`, `cold.fan`: leave alone, auto, 1–5, or quiet. Applied on entering the state and resent after each defrost. "Leave alone" never touches the fan. A fan the unit doesn't have (2 or 4 on a 3-speed unit, auto or quiet on a unit with neither) counts as "leave alone".

Each setting can also be "Outdoor": the fan then follows the outdoor temperature (smoothed, as for the curve) through the state's map, `idle.fanmap`, `demand.fanmap` or `cold.fanmap`. A map is `outdoor:fan,...` with increasing temperatures, up to 8 points, the fan being `A` (auto), `1`–`5` or `Q` (quiet). The fan is that of the highest point at or below the outdoor temperature; below the first point it is the first point's. So `-5:5,0:4,5:3` is fan 5 below 0 °C, 4 from 0 °C and 3 from 5 °C.

- The fan is set at once on entering the state, and after a change of the map. After that each fan is kept for `fan.hold` (default 60 min) at least, so it does not flap around a point. The hold counts within a state only: a change of state (to idle, demand or target control, also by way of no sensor) applies the new state's fan at once.
- No change during a defrost or the hold after it.
- Empty or invalid map (logged), or outdoor temperature unknown → leave alone.

Defaults: `demand.fan` is "Outdoor" with `demand.fanmap` `-20:4,-6:3,1:2,7:1`, so fan 4 below −6 °C, 3 from −6 °C, 2 from 1 °C and 1 from 7 °C. On a 3-speed unit fans 2 and 4 are left alone, so set another map there. `cold.fan` defaults to 4 (left alone on a 3-speed unit) and `idle.fan` to auto (left alone on a unit with no auto fan).

## Outside changes

If the unit reports a demand, setpoint, QO or fan that differs from what the controller wants (IR remote, Onecta, the unit silently reverting demand to 100), the controller resends its values, at most once a minute per value, and logs that it overrode an outside change. Econo is forced off while the controller runs (Econo and demand share `D7`).

## Removed from Faikout's auto mode

- Timed on/off (`auto.0`, `auto.1`), auto power on/off by temperature (`auto.p`, `auto.ptemp`), heat/cool switching (`lockmode`), auto fan (`auto.fmax`, `no.fanauto`), the Faikout BLE remote (all of it; BLE is only for room sensors), the target range of the MQTT `control` command (its `env` stays).
- Settings: `thermostat`, `temp.track`, `temp.adjust`, `temp.noflap`, `heat.over`/`back`, `cool.over`/`back`, `switchtemp`, `pushtemp`, `t.predicts`, `t.predictt`, `t.sample`, `auto.lcontrol`, `t.coolmin`, `t.heatmax` and anything else only the old loop used.
- Kept: `auto.e` (now the controller's on switch), `auto.t`, `auto.b`, `auto.topic`, `auto.payload`, `t.min`, `t.max`, `t.control`, `frosttemp` (separate safety net), `min.outside`.

## Visibility

- **Log:** every state or sub-state change with its reason, e.g. `heatctl: DEMAND -> IDLE (room 21.6 > target 21.0+0.5)`; a status line every 10 min.
- **MQTT:** `info/<host>/heatctl` on every transition (state, sub-state, reason, inputs). Status JSON gains `ctl` (state), `ctl_sub`, `ctl_target`, `ctl_curve`, `ctl_heatw`, `ctl_i`, and `ctl_resumed` for 10 minutes after a resume.
- **Web:** the old auto section is replaced by an enable switch, a target slider and a status line such as "Demand 65% · target 21.3 (curve +0.30) · heat 410 W" or "Target control · setpoint 22.0 · ❄ defrost". Manual temperature, demand and QO controls are greyed out while the controller runs.
- **Home Assistant:** an enum sensor "Control state"; `hvac_action` reports `defrosting` / `idle` / `heating`; the climate target sets `auto.t`. Tuning entities: number entities for `auto.t`, `pid.kp`, `pid.ti`; sensors for the heat request (W), integral and effective target.

## Code layout

- `ESP/main/heatctl.c` / `heatctl.h`: plain C, `float`, no ESP-IDF includes. `heatctl_step(&state, &params, &in, now)` → outputs (state, sub-state, demand, QO, setpoint, fan, effective target, heat request W, heat target W, heat deficit Wh, integral, transition reason). Also parses the table and the curve. `heatctl_keep` writes the block kept over a restart and `heatctl_resume` hands it back after a boot. Runs as a host unit test.
- `ESP/main/Faikout.c`: the old local auto block is removed. Each control tick gathers inputs, calls `heatctl_step`, applies outputs with the existing `daikin_set_*` setters (resend works because the S21 reports overwrite `daikin.*` when the unit disagrees). No S21 protocol logic changes.
- Settings in `ESP/main/settings.def` only.

## Tests

- `ESP/main/test/heatctl_test.c`, run by `make -C ESP test`: every transition with hysteresis and minimum times; defrost enter / exit / cap; post-defrost hold; watchdog; stale sensor and NOSENSOR; integral start values after each transition; anti-windup; rate limits; QO per level and `demand.full`; table and curve parsing including bad input. The heat deficit: the mean heat given is the heat request between two levels, with and without a heat output estimate, with a wrong `demand.cop`, and through defrosts at two cadences; jumps over several levels; no wind-up at the top level; and a house with hourly defrosts does not hunt. Heating off and on again: the integral is kept, also through the rest and over a restart, the level is by the learnt heat per watt, and a house that a burst of other heat turned heating off in is back at the target two hours after heating comes on. The block kept over a restart: resumes in DEMAND, COLD, IDLE and the rest; nothing kept when off or in NOSENSOR; kept while the unit's state is unknown after a boot, over a second restart too; and every reject (checksum, magic, version, age, non-finite or out-of-range values, a level gone from the table) starts fresh.
- A small first-order room model inside the test to check the loop settles without hunting between DEMAND and IDLE.
- Firmware builds clean with `make s3` and `make pico`. Hardware testing is done by a human.
