# Säädin

Local ESP32 firmware for Daikin heat pumps and air conditioners. MQTT, Home Assistant and BLE room sensors, no cloud.

Säädin is a fork of [Faikout](https://codeberg.org/RevK/ESP32-Faikout) by Adrian Kennard ([RevK](https://codeberg.org/RevK)). The aim is to run the heat pump at a steady, modulated output: the firmware sets the unit's power limit ("demand") from a room temperature control loop instead of switching it on and off around a setpoint.

> **Status: early fork.** Apart from the [changes listed below](#changes-from-faikout), the code is still upstream Faikout. The app is named `Saadin`, but most source files and settings still use Faikout names. There are no Säädin releases yet.

## Credit

Almost all of this code is Faikout, written by Adrian Kennard and sponsored by [Andrews & Arnold Ltd](https://www.aa.net.uk/), with contributions from others. That includes:

- the reverse-engineered Daikin S21 and X50A protocols
- the web interface
- MQTT and Home Assistant integration
- BLE sensor support
- the ESP32 support library

Säädin only builds on that work.

The original project:

- Firmware and documentation: [codeberg.org/RevK/ESP32-Faikout](https://codeberg.org/RevK/ESP32-Faikout)
- Hardware: Faikout boards from [shop.revk.uk](https://shop.revk.uk/). Buying them supports the original author.
- Bundled libraries, all by RevK and kept with their own licences:
  - [ESP32-RevK](https://codeberg.org/RevK/ESP32-RevK) (`ESP/components/ESP32-RevK`)
  - [ESP32-BLE-Env](https://codeberg.org/RevK/ESP32-BLE-Env)
  - [AJL](https://codeberg.org/RevK/AJL)
  - [AXL](https://codeberg.org/RevK/AXL)
  - [SQLlib](https://codeberg.org/RevK/SQLlib)
  - [stringdecimal](https://codeberg.org/RevK/stringdecimal)

The commit history from the original project is kept, so each upstream author's work is still attributed to them.

**Please do not report Säädin problems to the Faikout project.** If you can reproduce a problem with unmodified Faikout firmware, report it upstream; otherwise open an issue here.

## Not affiliated

Säädin is an independent fork. It is not affiliated with, endorsed by or supported by Andrews & Arnold Ltd, Adrian Kennard or Daikin.

- *Faikout* and the A&A and AJK logos are trademarks of Andrews & Arnold Ltd.
- *Daikin* is a trademark of Daikin Industries, Ltd.

These names appear here only to describe where the code comes from and what it works with.

## Hardware

Säädin is firmware only. It is meant to run on [Faikout boards](https://shop.revk.uk/), which plug into the Daikin's S21 connector (or X50A on larger and ducted units). This repository has no hardware designs. The KiCad PCB designs, case models and production files are in the [upstream repository](https://codeberg.org/RevK/ESP32-Faikout).

**Safety:** always isolate the power before opening an air conditioner. Some connector types carry dangerous voltages.

## Features (inherited from Faikout)

- Local web control with live status over a websocket. No app and no internet connection needed.
- MQTT reporting and control.
- Home Assistant auto-discovery over MQTT.
- Detects automatically whether the unit uses the S21 or X50A protocol.
- BLE temperature sensors (BlueCoinT, Telink/Xiaomi) can be used as the room reference for automatic control.
- Linux MySQL/MariaDB logging and graphing tools (`Tools/`).

## Planned

- **Heating control, next steps:** slow on/off cycling below the lowest demand, time-proportioning in the gap between 95% and 100% with quiet outdoor, and optional outdoor-temperature feedforward.
- **Releases:** builds published as GitHub Releases, with Säädin naming throughout.

## Building

Build and flash from source with [ESP-IDF](https://docs.espressif.com/projects/esp-idf/). [mise](https://mise.jdx.dev/) sets up the tools:

```sh
mise install          # eim (ESP-IDF installer), prek, and popt for the host tools
eim install           # once: install ESP-IDF; `eim select` picks the version
```

In this directory mise then puts the selected ESP-IDF on your PATH, so no `source …/activate_idf_*.sh` is needed. Without mise, activate ESP-IDF yourself and install popt (e.g. `brew install popt`).

```sh
cd ESP
make pico     # original Faikout board (ESP32-PICO)
make s3       # ESP32-S3 boards
make s3eth    # ESP32-S3 Ethernet board
make flash
```

`ESP/Makefile` has targets for the other chips.

Libraries are included as git subtrees, so a plain clone is enough; `--recursive` is not needed. Boards can be flashed through the USB test pads, or with a [TC2030-USB-NL](https://www.tag-connect.com/product/tc2030-usb-nl) lead on boards with a Tag-Connect footprint.

> **No OTA downloads, no signing.** Säädin never fetches firmware from a server. There is no OTA host, no automatic update check, and no `upgrade` command or button. Builds are not signed. To update, download a release and upload the `.bin` on the "Firmware" settings tab.
>
> A device running upstream Faikout rejects Säädin images over OTA, because they are unsigned and have a different app name. Flash the first Säädin build over USB. To go back, upload an upstream Faikout `.bin` on the "Firmware" tab; Säädin accepts it.

## Set-up and documentation

When first started, the device acts as a Wi-Fi access point with a set-up page. After it joins your network, open `http://<hostname>.local/` for the control page.

These manuals come from upstream and still use the Faikout name:

- [Setup](Manuals/Setup.md)
- [Controls](Manuals/Controls.md)
- [Heating control](Manuals/Heating.md) (new in Säädin)
- [Advanced](Manuals/Advanced.md)
- [BLE sensors](Manuals/BLE.md)
- [S21 protocol](Manuals/S21.md). The [upstream wiki](https://codeberg.org/RevK/ESP32-Faikout/wiki/S21-Protocol) is more up to date.

## Changes from Faikout

As required by GPL-3.0 §5(a), this section lists how Säädin differs from upstream Faikout.

**2026-11-10**
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

**2026-10-04**
- Fixed a memory leak on every view of the settings page that shows a drop-down (enum) setting, such as the fan type.
- Fixed heap corruption when the Library settings page shows a stored certificate or key.
- Fixed a crash when the settings page or main page lists BLE sensors while the device drops a stale sensor from its list.
- Fixed a deadlock that could hang the web server and the main loop while a BLE Faikout remote changes the aircon and a browser has the main page open.
- Fixed the legacy `/aircon/set_control_info` API. A request that set the mode got no reply, leaked memory, and ignored the other values in the request (such as temperature and fan).
- Fixed the web server locking up for good (connection reset, while the aircon keeps working) when several WebSocket connections close at once.
- Builds are no longer signed, and the firmware no longer requires signed OTA images.
- Removed OTA downloads. The firmware no longer checks for or fetches updates from a server, and the `upgrade` command, the upgrade button and the `ota.*` settings are gone.
- Added firmware upload from the browser: the "Firmware" tab on the settings page (`/revk-ota`).

**2026-10-03**
- Forked from upstream Faikout.
- Removed the PCB designs, case models and product photos, which are still available upstream.
- Removed the release binaries and the A&A Declaration of Conformity.
- Removed the Makefile targets that published releases to the upstream OTA server.
- Removed these files from the git history as well.
- Rewrote this README.

## Security

Report vulnerabilities privately through GitHub's [private vulnerability reporting](https://github.com/rolloutoy/saadin/security/advisories/new), not in public issues.

## Licence

GPL-3.0, the same licence as Faikout. See [LICENSE](LICENSE).

- Copyright in the upstream code belongs to Adrian Kennard, Andrews & Arnold Ltd and the other Faikout contributors.
- Changes made in Säädin are released under the same licence.
