# Säädin

Local ESP32 firmware for Daikin heat pumps and air conditioners. MQTT, Home Assistant and BLE room sensors, no cloud.

Säädin is a fork of [Faikout](https://codeberg.org/RevK/ESP32-Faikout) by Adrian Kennard ([RevK](https://codeberg.org/RevK)). The aim is to run the heat pump at a steady, modulated output: the firmware sets the unit's power limit ("demand") from a room temperature control loop instead of switching it on and off around a setpoint.

> **Status: early fork.** Apart from the [changes listed below](#changes-from-faikout), the code is still upstream Faikout. Identifiers in the code (project name, binaries, hostname, MQTT topics, Home Assistant entities) still say `Faikout` and will be renamed later. There are no Säädin releases yet.

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

- **Demand control:**
  - The Daikin gets a fixed setpoint.
  - A PI controller driven by a BLE room sensor sets the demand limit (30–100%).
  - Optional outdoor-temperature feedforward.
  - Slow on/off cycling below minimum demand.
- **Fixes to the existing auto mode:** stop it controlling against a stale BLE reading, and let it power off on overshoot when `lockmode` is set.
- **Safer updates:** firmware upload from the browser and automatic rollback after a bad OTA.
- **Releases:** builds published as GitHub Releases, with Säädin naming throughout.

## Building

Build and flash from source with [ESP-IDF](https://docs.espressif.com/projects/esp-idf/):

```sh
cd ESP
make pico     # original Faikout board (ESP32-PICO)
make s3       # ESP32-S3 boards
make s3eth    # ESP32-S3 Ethernet board
make flash
```

`ESP/Makefile` has targets for the other chips.

Libraries are included as git subtrees, so a plain clone is enough; `--recursive` is not needed. Boards can be flashed through the USB test pads, or with a [TC2030-USB-NL](https://www.tag-connect.com/product/tc2030-usb-nl) lead on boards with a Tag-Connect footprint.

> **Turn off automatic updates.** By default the firmware checks the upstream OTA server (`ota.faikin.uk`) about once a week. It installs upstream Faikout whenever the version differs from what is running, which replaces Säädin. Set `ota.auto` to off, or point `ota.host` at your own server, before relying on a Säädin build.

## Set-up and documentation

When first started, the device acts as a Wi-Fi access point with a set-up page. After it joins your network, open `http://<hostname>.local/` for the control page.

These manuals come from upstream and still use the Faikout name:

- [Setup](Manuals/Setup.md)
- [Controls](Manuals/Controls.md)
- [Advanced](Manuals/Advanced.md)
- [BLE sensors](Manuals/BLE.md)
- [S21 protocol](Manuals/S21.md). The [upstream wiki](https://codeberg.org/RevK/ESP32-Faikout/wiki/S21-Protocol) is more up to date.

## Changes from Faikout

As required by GPL-3.0 §5(a), this section lists how Säädin differs from upstream Faikout.

**2026-10-04**
- Fixed a memory leak on every view of the settings page that shows a drop-down (enum) setting, such as the fan type.
- Fixed heap corruption when the Library settings page shows a stored certificate or key.
- Fixed a crash when the settings page or main page lists BLE sensors while the device drops a stale sensor from its list.

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
