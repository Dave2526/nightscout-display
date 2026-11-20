# Nightscout Display (LilyGO T-Display-S3)

*[Deutsche Version](README.md)*

Firmware for the [LilyGO T-Display-S3](https://lilygo.cc/products/t-display-s3) that shows the
current blood glucose from [Nightscout](https://nightscout.github.io/): BG in a 7-segment style
(colored by range), trend arrow, delta, IOB/COB and the history graph.

- Values are fetched via **HTTPS directly from Nightscout** (token with role `readable`), so the
  display works on any Wi-Fi network, including on the go.
- **MQTT** is optional and only used for control and status. Without a broker the display works
  as usual.
- Up to **3 Wi-Fi networks**, setup via its own access point, settings via a web interface.

> This GitHub repository is a mirror. Development happens on a private Forgejo instance, every
> change is pushed here from there. Issues are welcome here; pull requests I apply by hand, since
> they would otherwise be overwritten by the next mirror push.

## Build and flash

Built with [PlatformIO](https://platformio.org/) (`platformio.ini` included, e.g. in VS Code with
the PlatformIO extension).

- **First time via USB:** `NS_OTA_PASS=<password> pio run -e tdisplay-s3 -t upload`
  If no port shows up: hold BOOT, press RST.
- **Afterwards over Wi-Fi:** `NS_OTA_PASS=<password> NS_HOST=<hostname-or-ip> pio run -e ota -t upload`

> ⚠️ Always set `NS_OTA_PASS` and use the same password for later updates. Without a password,
> anyone on the same Wi-Fi can flash a different firmware.

### Your own defaults

Copy `src/config_example.h` to `src/config_local.h` and adjust it: Nightscout URL, MQTT server,
topic prefix and the targets of the network test. `config_local.h` is listed in `.gitignore`.
All values can also be changed later in the web interface.

## Setup

- Nightscout token: in Nightscout, open *Admin Tools*, create a subject with the role `readable`
  and use its access token.
- If the display finds none of its Wi-Fi networks, it opens the access point `NS-Display`
  (192.168.4.1). Enter Wi-Fi, Nightscout URL and token there. The new network is added at the
  top; with all 3 slots used, the oldest one is dropped.
- Web interface `http://<hostname>/`: Wi-Fi networks, thresholds (defaults from Nightscout, button
  "load from Nightscout"), colors per range and the stored **defaults** (brightness, night mode,
  history, MQTT, hostname, NTP, time zone). Saving restarts the display.

The web interface and the on-screen texts are in German.

## MQTT (one prefix per device, e.g. `/room1/display`)

Status, all retained, one topic per value:

| Topic | Content |
|---|---|
| `<prefix>/status` | `online`/`offline` (LWT) |
| `<prefix>/sgv`, `/delta`, `/direction`, `/age_min` | BG (mg/dl), change, trend, age in minutes |
| `<prefix>/iob`, `/cob` | from Nightscout |
| `<prefix>/error` | empty or error text (`Token?`, `Verbindung` …) |
| `<prefix>/error_detail` | for `Verbindung`: HTTP error code and TLS error |
| `<prefix>/power`, `/brightness`, `/night`, `/night_active` | `ON`/`OFF`, 0–255, `ON`/`OFF`/`AUTO`, `ON`/`OFF` |
| `<prefix>/ip`, `/ssid`, `/rssi`, `/version` | device |
| `<prefix>/gateway`, `/dns`, `/subnet`, `/bssid`, `/time` | network and time (for troubleshooting) |
| `<prefix>/nettest` | result of the network test |

Commands:

| Topic | Value |
|---|---|
| `<prefix>/set/brightness` | `0`–`255` or `default` |
| `<prefix>/set/power` | `ON`/`OFF` |
| `<prefix>/set/night` | `ON`/`OFF`/`AUTO` |
| `<prefix>/set/refresh` | fetch now |
| `<prefix>/set/nettest` | TCP connection test to the targets in `config_local.h` |
| `<prefix>/set/reset` | back to defaults |

Commands only apply until the display is unplugged (then the defaults are used again). **Do not
send them retained**, retained commands are ignored.

## Buttons

- left short: brightness (steps, or switch on) · long: rotate screen
- right short: status page (IP, Wi-Fi, MQTT, Nightscout, uptime; returns after 30 s) · hold 5 s:
  reset Wi-Fi/token

## Disclaimer

This display is for information only and **not a medical device**. Never make treatment
decisions based on it alone.

## License

[MIT](LICENSE)

---

Built with [Claude](https://claude.ai) (Anthropic).
