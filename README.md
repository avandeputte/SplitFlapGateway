# Split-Flap Gateway

> ### 🌐 [splitflap.iothing.net](https://splitflap.iothing.net) · 📖 [Documentation](https://splitflap.iothing.net/docs/)
> Quick start · choosing a configuration · provisioning & calibration · the SplitFlap,
> Matrix and LCD gateways · the companion and its apps · APIs and wire protocols — the
> whole ecosystem, documented in one place.

[![Latest release](https://img.shields.io/github/v/release/avandeputte/SplitFlapGateway?label=firmware&color=f5b942)](https://github.com/avandeputte/SplitFlapGateway/releases/latest)

Firmware for the **Waveshare ESP32-S3-RS485-CAN** that puts a mechanical split-flap display on
your network. It bridges the modules' RS-485 bus to WiFi and offers a web dashboard to
provision, calibrate, diagnose and drive the wall, a REST API, MQTT with Home Assistant
discovery, and over-the-air updates. It is the middle piece of the SplitFlap ecosystem,
between the [module firmware](https://github.com/avandeputte/SplitFlapUniversalFirmware) and
the [companion app](https://github.com/avandeputte/SplitFlapGatewayCompanion) that decides
what the wall shows. The [blog post](BLOG.md) tells the story; the
**[documentation](https://splitflap.iothing.net/docs/)** covers everything else.

## Hardware

**[Waveshare ESP32-S3-RS485-CAN](https://www.waveshare.com/wiki/ESP32-S3-RS485-CAN)**
([Amazon](https://www.amazon.com/dp/B0FNCWZ3D1)). RS-485 on GPIO 17 (TX), 18 (RX), 21 (DE/RE);
PCF85063 RTC on I²C (SDA 39, SCL 38); debug serial on the native USB port at 115200 baud. The
bus runs at 9600 baud, 8N1.

[`SplitFlapGatewayCase.3mf`](SplitFlapGatewayCase.3mf) is a printed case that replaces the
Waveshare enclosure, clips the gateway onto the modules' DIN rail and takes 12 V and the bus
from the conductor rail through pogo pins — see
[Gateway Case](https://splitflap.iothing.net/docs/Gateway-Case/).

## Installing

- **Prebuilt image, no toolchain.** Every [release](https://github.com/avandeputte/SplitFlapGateway/releases/latest)
  carries `SplitFlapGateway-<version>.factory.bin` for a blank board (flash at `0x0`, for
  instance with [Espressif's browser flasher](https://espressif.github.io/esptool-js/)) and
  `SplitFlapGateway-<version>.bin` for updating a running gateway from its dashboard
  (**Settings ▸ Open Firmware Updater**).
- **From source.** A [PlatformIO](https://platformio.org/) project: `pio run -t upload` builds
  and flashes over USB, `pio run -e esp32s3_ota -t upload` pushes an update over WiFi.
  [SETUP.md](SETUP.md) is the from-nothing walkthrough.

**First boot.** With no WiFi configured the gateway raises its own access point,
`Split-Flap-GW` / `12345678`; open http://192.168.4.1, enter your WiFi under Settings, and it
joins your network (it then appears as `splitflap-gw-xxxxxx.local`). The full set-up, and what
to do when a web update fails, is in
[the gateway guide](https://splitflap.iothing.net/docs/SplitFlap-Gateway/).

## Interfaces

- **Web dashboard** served by the board itself, in 14 languages.
- **REST API** — [`openapi.yaml`](openapi.yaml) is the full specification (import it into
  Postman or Swagger UI); the [Gateway API](https://splitflap.iothing.net/docs/Gateway-API/)
  page is the readable reference.
- **MQTT** — bus frames and module events published under `splitflap/`, control topics, and
  optional Home Assistant auto-discovery.
- **RS-485** — the module wire protocol, documented in
  [Bus Protocol](https://splitflap.iothing.net/docs/Bus-Protocol/).

## In this repository

| | |
|---|---|
| [`src/`](src/) | The firmware ([ARCHITECTURE.md](ARCHITECTURE.md) explains the non-obvious decisions) |
| [`ui/`](ui/) | The dashboard page and its translations; `tools/build_ui.py` bakes them into the firmware |
| [`openapi.yaml`](openapi.yaml) | The REST API specification |
| [`SETUP.md`](SETUP.md) | Building and flashing from source |
| [`RELEASE_NOTES.md`](RELEASE_NOTES.md) | What changed in each version (also on the [Releases](https://github.com/avandeputte/SplitFlapGateway/releases) page) |
| [`BLOG.md`](BLOG.md) | Why the project exists |
| [`SplitFlapGatewayCase.3mf`](SplitFlapGatewayCase.3mf) | The rail-clip case |

## License

[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/) — share and adapt for
non-commercial purposes, with credit, under the same license. Software © Alex Van de Putte.
The split-flap module hardware and the original bus protocol are by
[Adam G Makes](https://www.youtube.com/@AdamGMakes).
