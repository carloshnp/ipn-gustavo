# Firmware Wi-Fi Provisioning (Serial CFG)

This document is specific to your Mega firmware implementation.

## Preconditions
- Firmware flashed successfully
- ESP8266 connected to `Serial1`
- Serial monitor at `115200`
- API already deployed (know `API_HOST`, token)
- If one-shot deploy auto-generated token, use the exact token printed in `CFG API_TOKEN ...`

## Required commands
Send line-by-line:

```text
CFG WIFI_SSID <your_ssid>
CFG WIFI_PASS <your_password_or_blank_for_open_wifi>
CFG API_HOST <api-id>.execute-api.us-east-1.amazonaws.com
CFG API_PATH /v1
CFG API_TOKEN <token>
CFG DEVICE_ID MEGA001
CFG WIFI_ENABLE 1
CFG SAVE
CFG TEST
CFG SHOW
```

## Open Wi-Fi example
For open networks without password:

```text
CFG WIFI_SSID <your_open_ssid>
CFG WIFI_PASS
CFG SAVE
CFG TEST
```

## Command reference
- `CFG SHOW`: prints active cloud config
- `CFG SAVE`: persists config in EEPROM
- `CFG TEST`: forces reconnect cycle immediately
- `CFG WIFI_ENABLE 0`: disables cloud upload (SD-only mode)

## Validation steps
1. Open Service -> `WiFi Status`.
2. Confirm state transitions:
   - `WF:CON` then `WF:ON`
3. Check HTTP code and counters:
   - success codes in 2xx range
   - sent counter increments
4. Run an experiment and confirm:
   - SD logging works always
   - cloud counters progress when network is available

## If it fails
- Verify SSID/password
- Verify API host/path/token
- If token was auto-generated, confirm there are no missing/truncated characters in `CFG API_TOKEN`
- Verify TLS availability and internet connectivity
- Confirm SG on EC2/API allows your source path as designed
- Open network with captive portal is not supported by this AT flow. Use open SSID without portal authentication.

## Return to SD-only mode
```text
CFG WIFI_ENABLE 0
CFG SAVE
CFG SHOW
```
