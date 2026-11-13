# Changelog

Every user-visible change in Säädin since it was forked from upstream Faikout, newest first. As required by GPL-3.0 §5(a), this is the dated record of how Säädin differs from Faikout. [README.md](README.md#changes-from-faikout) has a summary.

## 2026-11-12

- The heating controller now carries on where it was after a restart of the device, in place of starting again in demand control at `demand.start` (60%). It keeps its state (demand control, cold-weather mode or heating off), the demand level, the integral, the learnt heat per watt, the heat deficit and, in cold-weather mode, the setpoint, in memory that survives a restart but not a power cut. Minimum times and holds start again. It starts afresh as before after a power cut, after more than 30 minutes, when it was off or had no room reading before the restart, or when what it kept no longer fits the settings. The reason for the change says "resumed after restart", or why it did not resume; the web status line shows "resumed after restart" for 10 minutes, and the status JSON has `ctl_resumed`. Nothing is written to flash.
- The heating graph is drawn anew, as strips stacked on one time axis in place of four charts with up to three scales each. Each strip has one scale and all are the same width, so a defrost lines up down the page.
  - The strips are in five groups. *Room*: room temperature, target and effective target, and the outdoor temperature, which is now always shown. *Heat wanted and given*: heat request, integral, heat target, the demand level as the heat it is taken to give, heat output and its 2 hour mean, and the heat deficit. *Compressor side*: the demand level, power with the level's watts in `demand.table`, and compressor frequency. *Air side*: liquid temperature, the unit's room reading and fan speed. *Efficiency*: COP and its 6 hour mean.
  - The demand level is shown by name (`95`, `100q`, `100`) in a band, in place of the demand % line, which could not tell 100% with quiet outdoor from plain 100%.
  - There is one room temperature line, the reading as the controller sees it after `auto.smooth`. The raw reading is still in `/graph.json` as `room`.
  - Three new charts sum up the range shown: the power drawn at each demand level against `demand.table`, the COP of each defrost cycle by outdoor temperature, and the room temperature through each defrost.
  - A strip at the top shows the whole log: drag across it to choose the range. On the strips, a drag now pans (it used to zoom in on the range dragged across, with shift+drag to pan), ctrl+scroll or a pinch zooms as before, and a double-click shows the whole log.
  - Moving the pointer over a strip shows the values at that moment in every legend, and the state, demand level, fan setting, kp and ti in a line above the strips, in place of the tooltip.
  - *Refresh* now loads the log again without reloading the page, and keeps the range shown, a zoom made by dragging included.
  - *Saved log* shows a `/graph.json` file saved earlier in place of the device's log.
  - `/graph.json` has a new field `table`: the demand table in use, as `demand.table` is written.
  - The page is now drawn with [d3](https://d3js.org) 7.9.0 (ISC licence), built into the firmware, in place of Chart.js, chartjs-plugin-zoom and Hammer.js. The firmware is about 15 KB larger.
- Demand control now works on heat, not electrical power. The PI loop's output, now called the heat request (was heat wanted), is in watts of heat. Every second the controller adds the heat request less the estimated heat output to the heat deficit, and steps demand between the levels around the heat request so that the deficit stays near nothing. Before, it used the highest level whose watts in `demand.table` were at or below the heat wanted, so with the big gaps at the top (95% → 100% with quiet outdoor → plain 100% draw about 680 → 1350 → 3000 W) the room had to stay off target for hours before demand moved a level.
  - Heat not given during a defrost and the hold after it is made up over the next two hours or so: it is added to the heat request to give the heat target, so more defrosts mean more heat between them.
  - `pid.kp` is now watts of heat per degree, and its default is 900 (was 300 electrical watts). **A device with its own `pid.kp` keeps the number, which is now about a third as strong: multiply it by 3.**
  - New setting `demand.cop` (default 3.0): heat per watt of `demand.table`, roughly. The controller learns the real figure from the heat output estimate. Without the estimate (`fan.flow` blank) it uses the table's watts times `demand.cop` as the heat given.
  - Expect more demand changes in cold weather than before, at most one per `demand.rate`.
  - The second chart of the heating graph now has the heat request (as before, under its new name), the heat target, the 2 hour mean of the estimated heat output and the heat deficit, in place of the power; the proportional part is on the same axis as the rest. The fourth chart has the heat per watt of the demand table that the controller has learnt. `/graph.json` has the new columns `deficit`, `heattarget` and `scale`. The log now takes about 1.4 MB of PSRAM (was 1.3 MB).
  - The `info/<host>/heatctl` message has `deficit`. `ctl_heatw` and `ctl_i` in the status JSON are now watts of heat, and the Home Assistant sensor for `ctl_heatw` is now named "Heat request". The names `ctl_heatw` and, in `/graph.json`, `heatw` are unchanged.
- The rest after a defrost loop (`defrost.watchdog`) now sets the indoor fan to auto, whatever `idle.fan` is. When heating resumes, the fan for that state (`demand.fan` or `cold.fan`) is set again; if that is *Leave*, the fan stays on auto.
- The outdoor temperature the heating controller uses is now held still for the hold after a defrost (`defrost.hold`) too, not only during the defrost, and a fan set by outdoor temperature does not change in that hold. The unit's outdoor reading spikes right after a defrost, which could change the fan and then keep it for `fan.hold`. A change of state (to idle, demand or full power) still sets that state's fan at once. With `defrost.hold` at 0 there is no hold, so nothing is held.
- The mean COP on the heating graph is now over the last 6 hours instead of the last hour. It still shows once there is half an hour of samples.

## 2026-11-11

- Heating control can set the fan by outdoor temperature. `demand.fan`, `idle.fan` and `cold.fan` have a new choice *Outdoor*, which takes the fan from the new settings `demand.fanmap`, `idle.fanmap` and `cold.fanmap` (`outdoor:fan,...`, for example `-5:5,0:4,5:3`). Each fan is then kept for `fan.hold` (default 60 minutes) at least, so it does not flap. `demand.fan` now defaults to *Outdoor*, with the map `-20:4,-6:3,1:2,7:1`: fan 4 below −6 °C, 3 from −6 °C, 2 from 1 °C and 1 from 7 °C. It used to default to leaving the fan alone; set `demand.fan` to *Leave* for that. `cold.fan` now defaults to fan 4 and `idle.fan` to auto, also in place of leaving the fan alone.
- The heating graph keeps 5 days instead of 3 on boards with PSRAM, which takes about 1.3 MB of it, and has a *3 days* range button.
- Heating control now turns heating off (idle) only when demand is already at its lowest level. It used to do so from any level once the room was `idle.over` above the target, so a short overshoot at high demand in cold weather could switch heating off for `idle.min` and restart it from the lowest level.
- The heating graph can refresh itself: choose 15 s, 30 s, 1 min or 5 min under Refresh (off by default), or give another interval in the address (`/graph?refresh=120`). A refresh keeps the range chosen with the All / 1 day / 12 h / 3 h buttons.
- The heating graph now keeps one sample every 15 seconds instead of every 30, still for 3 days, which takes about 760 KB of PSRAM (was 380 KB). Boards without PSRAM keep 1.5 hours instead of 3, so the log uses the same memory there. Loading the graph no longer needs a second copy of the whole log in memory.
- The heating graph has a fourth chart with the estimated heat output and COP (each sample's, and the mean over the last hour with defrosts counted in). The estimate uses the indoor fan speed, the unit's own room reading and the liquid temperature; the new setting `fan.flow` gives the air flow at each fan speed; its default is for a Perfera 40 and needs adjusting for other models. The third chart now also shows the indoor fan speed the unit reports and the unit's own room reading. `/graph.json` has the new columns `fan`, `intake` and `heatout`. The tooltip and `/graph.json` (`fanset`, `quiet`) also give the fan setting and whether quiet outdoor is on, so 100% with quiet outdoor can be told from plain 100%. The log now takes about 380 KB of PSRAM (was 310 KB).
- Fixed the settings page failing to save with "Tag too long". The `defrost.watchdog` switch, added on 2026-11-10, has a name long enough to trip a length check in the settings form.
- New setting `defrost.holdmax` (default 100%): the highest demand in the hold after a defrost. 100 means 100% with quiet outdoor, so a unit that was at 100% with quiet outdoor before a defrost now stays there afterwards. It used to drop to 95% for the hold. Plain 100% is still not used in the hold. Set `defrost.holdmax` to 95 for the old behaviour.

## 2026-11-10

- Fixed a memory leak (about 1–3 KB) each time the settings page's "Save settings" or "Upload settings" button is used.
- The default `demand.table` is now `40:270,50:340,60:400,70:465,75:500,80:530,85:570,90:605,95:640,100q:1510,100:2470` (was `40:220,50:280,60:350,70:400,80:520,85:580,95:640,100q:1510,100:2470`). The lower levels match what the unit draws, and 75% and 90% make the steps more even, so the controller no longer sticks at 70% because 80% looks 120 W away. A device with its own `demand.table` keeps it.
- The defrost-loop watchdog (heating off for `defrost.rest` after more than `defrost.loops` defrosts in `defrost.window`) is now off by default. Turn it on with the new `defrost.watchdog` setting. With the old defaults it also fired on normal defrosts every 27 minutes around −2 °C, turning heating off for 15 minutes.
- `auto.smooth` now defaults to 0 (no room temperature smoothing) instead of 300 s. A sensor that reports in 0.01 °C steps barely flickers, so smoothing only added lag. A device that never changed `auto.smooth` switches to no smoothing on update; one with its own value keeps it.
- The heating graph now keeps one sample every 30 seconds instead of one a minute, still for 3 days. Boards without PSRAM keep 3 hours instead of 6, so the log uses the same memory there.
- During the hold after a defrost, the heating integral now keeps running. Demand (or the setpoint in cold-weather mode) still does not go up until the hold ends. Before, the integral was frozen during the hold too, so it learned slowly when defrosts were frequent.
- `pid.ti` and `cold.ti` of 0 now turn the integral off (P only), so the proportional gain can be tuned on its own. Before, 0 meant an integral time of one second. The Home Assistant "Heating integral time" number now goes down to 0.
- New heating graph page, `/graph`, linked from the bottom of the main page, above Settings. It shows up to the last 3 days of room temperature (raw and smoothed), target, effective target, setpoint, outdoor temperature, heat wanted and its integral and proportional parts, power, demand, liquid temperature and compressor frequency, with shading for heating off, cold weather, no room reading, defrosts and the hold and rest after them. Changes of kp or ti are marked, to help tune the demand controller. The device keeps one sample a minute in memory, 3 days of them (6 hours on boards without PSRAM), until it restarts. The charts zoom and pan together: drag across a chart to zoom in on that range, or ctrl+scroll, and shift+drag to pan; on a phone, pinch and drag. All, 1 day, 12 h and 3 h buttons pick a range, and a button longer than the log is hidden. The page is drawn with [Chart.js](https://www.chartjs.org) 4.5.1, [chartjs-plugin-zoom](https://www.chartjs.org/chartjs-plugin-zoom/) 2.2.0 and [Hammer.js](https://hammerjs.github.io) 2.0.8 (all MIT licence), which are built into the firmware. The samples are also available as JSON at `/graph.json`.
- The default timezone is now Finnish time (`EET-2EEST,M3.5.0/3,M10.5.0/4`) instead of UK time. A device that never changed `tz` switches to Finnish time on update; one with its own `tz` keeps it.
- The heating controller's full-power defaults are slower: `cold.hold` is 60 minutes (was 15) and `cold.rate` is 1800 s (was 600).
- Replaced Faikout auto with a new heating controller. [Manuals/Heating.md](Manuals/Heating.md) explains how to use it, and [docs/heat-control.md](docs/heat-control.md) has the design.
  - It runs only in heat mode, while `auto.e` is on and a target (`auto.t`) is set. `auto.e` is now off by default.
  - It sends the unit a fixed setpoint and limits demand to hold the room at the target. It uses the room temperature from a BLE sensor or MQTT.
  - When the room is warm, it turns heating off with a low setpoint.
  - Below −15 °C outside, it runs at demand 100% and controls the room with the setpoint.
  - It keeps its outputs as they are during a defrost. After several defrosts in a row, it turns heating off for a while.
  - If the room reading stops for 30 minutes, the unit's own thermostat takes over at the target.
  - It never turns the unit on or off, and leaves cool, dry and fan mode alone.
  - When it stops for any reason (switched off, unit off or out of heat mode), it sets demand back to 100%, quiet outdoor off, and the setpoint to `stop.temp` (default 22 °C). A setpoint someone set at the same moment is kept.
- New settings: `auto.curve` (target offset by outdoor temperature), `auto.stale`, `auto.smooth`, `stop.temp`, `demand.set`, `demand.table`, `demand.full`, `demand.start`, `demand.rate`, `demand.minrun`, `demand.fan`, `pid.kp`, `pid.ti`, `idle.temp`, `idle.over`, `idle.under`, `idle.min`, `idle.fan`, `cold.enter`, `cold.exit`, `cold.hold`, `cold.set`, `cold.rate`, `cold.kp`, `cold.ti`, `cold.fan` and `defrost.enter`, `defrost.exit`, `defrost.max`, `defrost.hold`, `defrost.loops`, `defrost.window`, `defrost.rest`.
- Removed support for the Faikout BLE remote. BLE is now only for room temperature sensors. A remote is ignored: it no longer changes the unit's power, mode, setpoint or fan, and Säädin no longer advertises its own state over BLE for a remote to show.
- Removed from Faikout auto: timed on/off, auto power on/off by temperature, heat/cool switching, auto fan, and the target range of the MQTT `control` command (its `env` still sets the room temperature). Removed settings: `auto.0`, `auto.1`, `auto.r`, `auto.p`, `auto.ptemp`, `auto.fmax`, `auto.lcontrol`, `lockmode`, `thermostat`, `thermref`, `temp.track`, `temp.adjust`, `temp.noflap`, `heat.over`, `heat.back`, `cool.over`, `cool.back`, `switchtemp`, `pushtemp`, `t.predicts`, `t.predictt`, `t.sample`, `t.coolmin`, `t.heatmax`, `no.fanauto`.
- Frost protection (`frosttemp`) still turns the unit on in heat mode, but no longer changes its setpoint.
- The web page's auto section is now an on switch, a target slider and a status line, for example "Demand 60% QO · target 21.3 (curve +0.30) · heat 350 W". Manual temperature, demand and quiet outdoor are greyed out while the controller runs. The settings page has the target curve, demand table and fan per state.
- The status JSON has `ctl`, `ctl_sub`, `ctl_target`, `ctl_curve`, `ctl_heatw` and `ctl_i`, and every change of state is sent as `info/<host>/heatctl` with the reason.
- Home Assistant gets a "Control state" sensor, sensors for heat wanted, the integral and the effective target, and number entities for the target, `pid.kp` and `pid.ti`. The climate's target sets `auto.t` while the controller is on and the unit heats. Its mode no longer shows `heat_cool` while controlling. `hvac_action` is now reported by default (`no.hvacaction` is off), showing defrosting, idle or heating.
- When a BLE room sensor stops reporting, its reading is dropped after 5 minutes instead of being kept.
- The MQTT `temp` command sets `auto.t` while the controller is on and the unit is in heat mode, like the Home Assistant climate target.
- `no.faikoutauto` (*Hide heating control*) now hides the heating controller from Home Assistant, and from the web page unless the controller is on and no settings password is set.
- Fixed a small memory leak when a `control` command has a field that fails.
- Firmware uploaded from the browser now starts on probation. Every page shows a banner with **Keep** and **Roll back now** buttons and a 10-minute countdown. Restarting from the web UI also keeps the firmware. If nobody keeps it in time, if it gets no IP address within 3 minutes, or if the device restarts any other way first (crash, power loss, MQTT `restart`), the device goes back to the previous firmware. The old firmware then says why the update was rolled back, until its next restart, and sends `info/upgrade` over MQTT. While new firmware is on probation, another upload is refused. This needs the new bootloader, flashed over USB once.
- The "Firmware" tab can boot the previous firmware, which then goes through the same probation.
- Faikout, or any other firmware that can't confirm itself, is installed without probation, so the bootloader doesn't switch it back on its next reset.
- Renamed the app from `Faikout` to `Saadin`. Binaries are `Saadin-<board>.bin`, the setup access point is `Saadin-…`, the Home Assistant device model is `Saadin`, and the app-wide MQTT topics use `Saadin` (for example `command/Saadin` for all devices). Settings are kept.
- The "Firmware" upload also accepts upstream Faikout images, so a device can go back to Faikout without USB.
- Reformatted all C code to ESP-IDF style. Only whitespace changed.
- Added clang-format and whitespace checks, run with prek.

## 2026-10-04

- Fixed a memory leak on every view of the settings page that shows a drop-down (enum) setting, such as the fan type.
- Fixed heap corruption when the Library settings page shows a stored certificate or key.
- Fixed a crash when the settings page or main page lists BLE sensors while the device drops a stale sensor from its list.
- Fixed a deadlock that could hang the web server and the main loop while a BLE Faikout remote changes the aircon and a browser has the main page open.
- Fixed the legacy `/aircon/set_control_info` API. A request that set the mode got no reply, leaked memory, and ignored the other values in the request (such as temperature and fan).
- Fixed the web server locking up for good (connection reset, while the aircon keeps working) when several WebSocket connections close at once.
- Builds are no longer signed, and the firmware no longer requires signed OTA images.
- Removed OTA downloads. The firmware no longer checks for or fetches updates from a server, and the `upgrade` command, the upgrade button and the `ota.*` settings are gone.
- Added firmware upload from the browser: the "Firmware" tab on the settings page (`/revk-ota`).

## 2026-10-03

- Forked from upstream Faikout.
- Removed the PCB designs, case models and product photos, which are still available upstream.
- Removed the release binaries and the A&A Declaration of Conformity.
- Removed the Makefile targets that published releases to the upstream OTA server.
- Removed these files from the git history as well.
- Rewrote the README.
