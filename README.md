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

- **Demand control:**
  - The Daikin gets a fixed setpoint.
  - A PI controller driven by a BLE room sensor sets the demand limit (30–100%).
  - Optional outdoor-temperature feedforward.
  - Slow on/off cycling below minimum demand.
- **Fixes to the existing auto mode:** stop it controlling against a stale BLE reading, and let it power off on overshoot when `lockmode` is set.
- **Safer updates:** firmware upload from the browser and automatic rollback after a bad OTA.
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
- [Advanced](Manuals/Advanced.md)
- [BLE sensors](Manuals/BLE.md)
- [S21 protocol](Manuals/S21.md). The [upstream wiki](https://codeberg.org/RevK/ESP32-Faikout/wiki/S21-Protocol) is more up to date.

## Changes from Faikout

As required by GPL-3.0 §5(a), this section lists how Säädin differs from upstream Faikout.

**2026-11-10**
- With `lockmode` and auto power on/off (`autop`) set, Faikout auto now turns the aircon off when the room overshoots the target range, instead of leaving it running because it may not swap heat/cool. It turns back on only once the room needs the locked mode again.
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
