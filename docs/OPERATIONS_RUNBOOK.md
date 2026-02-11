# Operations Runbook (SD-First Wi-Fi Cloud)

## Service health checks

## Device-side
- SD must stay authoritative:
  - `RUNxx.CSV` files continue growing during experiments.
- Cloud side-car sync files:
  - `RUNxx.ACK`, `EVENTS.ACK` should advance over time.
- Service menu:
  - `WiFi Status` shows network state and HTTP code.

## Backend-side
- Lambda logs: no sustained 4xx/5xx spikes.
- Timestream ingest and query latency stable.
- Streamlit accessible at EC2 `:8501`.

## Common maintenance

## Rotate API token
1. Generate new token.
   - Option A: rerun one-shot deploy and leave API token blank to auto-generate.
   - Option B: generate manually and use that value.
2. Update Lambda env var `API_TOKEN` (SAM redeploy or console).
3. Re-provision device:
   - `CFG API_TOKEN <new>`
   - `CFG SAVE`
   - `CFG TEST`
4. Verify uploads continue.

## Rotate dashboard password
1. Choose new password and compute SHA256.
2. Update `/etc/chamber-dashboard.env` on EC2:
   - `APP_PASSWORD_SHA256=<newhash>`
3. Restart service:
   - `sudo systemctl restart chamber-streamlit.service`

## Restart dashboard
```bash
sudo systemctl restart chamber-streamlit.service
sudo systemctl status chamber-streamlit.service
```

## Inspect dashboard logs
```bash
sudo journalctl -u chamber-streamlit.service -n 200
sudo journalctl -u chamber-streamlit.service -f
```

## Recovery procedures

## Wi-Fi/API failures while experiment runs
Expected behavior:
- no experiment freeze
- SD logging unaffected
- cloud retries with backoff

Actions:
1. Confirm SD writes are still progressing.
2. Check `WiFi Status` for state and last HTTP code.
3. Run `CFG TEST` from serial console.
4. Validate API endpoint and token.
5. For open Wi-Fi, set blank password explicitly:
   - `CFG WIFI_PASS`
   - `CFG SAVE`
   - `CFG TEST`
6. If the network requires captive-portal login, this firmware flow will not authenticate; use a non-portal network.

## Lambda errors
1. Open CloudWatch logs for ingest Lambda.
2. Check auth header presence:
   - `X-Api-Token`
   - `X-Device-Id`
3. Validate request JSON shape.

## Timestream write errors
1. Confirm Lambda role permissions:
   - `timestream:WriteRecords`
   - `timestream:DescribeEndpoints`
2. Confirm DB/table names passed in env vars.

## EC2 unreachable
1. Confirm instance state + status checks.
2. Verify SG ingress matches current client IP.
3. If client IP changed, update SG or rerun one-shot.

## Disaster / rollback
1. Stop cloud path without touching SD behavior:
   - `CFG WIFI_ENABLE 0`
   - `CFG SAVE`
2. Delete ingest stack and EC2 resources later.

## Performance and safety notes
- Do not make cloud upload blocking in control loops.
- Keep SD primary and immutable for completed runs.
- Keep cloud as eventually-consistent copy for monitoring.
