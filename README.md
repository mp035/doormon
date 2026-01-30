# Doormon

A small ESP32 firmware that connects to WiFi, runs an HTTP server, and reports a **triggered** state driven by a GPIO input. Useful as a simple door or motion monitor: when IO2 sees a falling edge, the device latches into “triggered”; clients poll `/status` or call `/reset` to clear it.

**Target:** DFRobot FireBeetle ESP32 (V4.0), built with ESP-IDF via PlatformIO.

## Features

- WiFi station mode with configurable SSID/password
- HTTP server on port 80 with two endpoints
- **IO2 (GPIO 2)** as trigger input: falling edge latches `triggered`
- JSON responses for `/status` and `/reset`

## Hardware

- **Board:** [DFRobot FireBeetle ESP32](https://www.dfrobot.com/product-1590.html) (or compatible)
- **Trigger input:** IO2 → GPIO 2, configured as input with internal pull-up. A falling edge (e.g. contact closed, PIR pulse) sets the latched state.

## Requirements

- [PlatformIO](https://platformio.org/) (CLI or IDE)
- USB cable for flash and serial monitor

## Configuration

Edit `src/main.c` and set your WiFi credentials:

```c
#define WIFI_SSID      "YOUR_SSID"
#define WIFI_PASSWORD  "YOUR_PASSWORD"
```

Optional: adjust `WIFI_MAX_RETRY` (default 5) and `TRIGGER_GPIO` if you use a different pin.

## Build & Upload

```bash
# Build
pio run -e firebeetle32

# Upload firmware
pio run -e firebeetle32 -t upload

# Serial monitor (115200 baud)
pio device monitor
```

Or use the PlatformIO IDE tasks for your board.

## API

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET`  | `/status` | Returns `{"triggered": true \| false}`. Starts as `false`; becomes `true` after a falling edge on IO2 and stays until reset. |
| `GET`  | `/reset`  | Clears the triggered state, returns `{"reset": true}`. |
| `POST` | `/reset`  | Same as `GET /reset`. |

Responses are `application/json`.

## Example

```bash
# Check status (initially untriggered)
curl http://192.168.1.100/status
# {"triggered":false}

# ... falling edge on IO2 ...

curl http://192.168.1.100/status
# {"triggered":true}

# Clear latch
curl http://192.168.1.100/reset
# {"reset":true}

curl http://192.168.1.100/status
# {"triggered":false}
```

Replace `192.168.1.100` with your ESP32’s IP (shown in serial log at boot).

## mDNS (doormon.local)

The firmware advertises **doormon.local** on the LAN so you can reach the device by name (e.g. `http://doormon.local/status`) without knowing its IP. This needs the ESP-IDF mDNS component.

PlatformIO’s bundled ESP-IDF doesn’t ship it, so add it locally:

```bash
git submodule add https://github.com/espressif/esp-protocols.git components/esp-protocols
git submodule update --init --recursive
```

Then build as usual. After flashing, the device will appear as **doormon.local** on the same LAN.

For a host-side monitor that discovers via mDNS and polls every second (reporting triggers and slow responses), see **scripts/doormon_monitor.py** and `scripts/README.md`.

## License

Use and modify as you like. No warranty.
