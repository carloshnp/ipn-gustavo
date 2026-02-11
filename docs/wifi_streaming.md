# Wi-Fi Streaming (SD-first)

## Principle
- SD logging is authoritative and permanent.
- Wi-Fi/cloud streaming is best-effort and non-blocking.
- If network fails, experiment continues and data remains on SD.

## Firmware commands over USB Serial (115200)
- `CFG SHOW`
- `CFG WIFI_SSID <ssid>`
- `CFG WIFI_PASS <password>`
- `CFG API_HOST <host>`
- `CFG API_PATH <base-path>` (example: `/v1`)
- `CFG API_TOKEN <token>`
- `CFG DEVICE_ID <id>`
- `CFG WIFI_ENABLE <0|1>`
- `CFG SAVE`
- `CFG TEST`

## SD sync sidecar
- For each `RUNxx.CSV`, uploader keeps `RUNxx.ACK` with:
  - `byteOffset,lineIndex,lastSyncEpoch`
- Events are appended to `EVENTS.CSV` and synced with `EVENTS.ACK`.
- No run file deletion is performed by firmware.

## API routes expected
- `POST /v1/telemetry/batch`
- `POST /v1/events/batch`

Headers:
- `X-Device-Id`
- `X-Api-Token`
- `Content-Type: application/json`

## Service menu
- New option: `WiFi Status`
- Displays Wi-Fi state, last HTTP status, pending estimate, sent/fail counters.
- `OK` triggers reconnect test.
