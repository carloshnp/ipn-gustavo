# One-Shot Deployment Tutorial (AWS + EC2 + Firmware)

This guide runs the complete SD-first cloud stack in one execution using PowerShell.

## 1. Prerequisites

## Local tools (Windows)
- PowerShell 5.1+ or PowerShell 7+
- AWS CLI v2 (`aws --version`)
- AWS SAM CLI (`sam --version`)
- Python 3.10+ (`python --version`)
- OpenSSH client (`ssh -V`)

## AWS requirements
- Region: `us-east-1`
- Existing EC2 key pair in `us-east-1`
- IAM permissions for:
  - CloudFormation, Lambda, API Gateway, IAM
  - EC2, SSM
  - Timestream write/query admin operations

## Firmware assumptions
- Mega firmware already flashed with `CFG` serial command support.
- ESP8266 AT firmware reachable over `Serial1`.
- SD is always primary persistence.

## 2. What the one-shot script creates
- Timestream DB and tables:
  - `${prefix}-chamber`
  - `${prefix}-telemetry`
  - `${prefix}-events`
- SAM stack `${prefix}-ingest` with:
  - HTTP API routes:
    - `POST /v1/telemetry/batch`
    - `POST /v1/events/batch`
  - Lambda ingest function
- EC2 (`t3.micro`) running Streamlit dashboard
- Security group restricted to your detected public IP:
  - TCP 22
  - TCP 8501
- Local receipt file:
  - `deploy/out/<prefix>-deployment.json`

## 3. Run one-shot deploy

From project root:

```powershell
cd C:\Users\carlo\OneDrive\Documentos\PlatformIO\Projects\260122-133648-megaatmega2560
.\deploy\one_shot_deploy.ps1
```

Optional flags:

```powershell
.\deploy\one_shot_deploy.ps1 -ProjectPrefix chamber-prod -Region us-east-1 -KeyPairName mykey
.\deploy\one_shot_deploy.ps1 -SkipEc2
.\deploy\one_shot_deploy.ps1 -ForceRecreateEc2
.\deploy\one_shot_deploy.ps1 -DryRun
```

During execution the script prompts for:
- project prefix
- key pair name
- API token (hidden; leave blank to auto-generate)
- Streamlit password (hidden)
- Wi-Fi SSID
- Wi-Fi password (optional; leave blank for open Wi-Fi)

If API token is left blank, the script generates a strong random token and uses it for deployment. The same token is printed later in `CFG API_TOKEN` and must be provisioned to the device.

## 4. Validate backend

## API health (manual check)
Use endpoint from script output (`ApiUrl`):

```powershell
$api = "https://<api-id>.execute-api.us-east-1.amazonaws.com/v1"
$headers = @{
  "X-Api-Token" = "<TOKEN>"
  "X-Device-Id" = "MEGA001"
  "Content-Type" = "application/json"
}
$body = '{"device_id":"MEGA001","records":[]}'
Invoke-WebRequest -Method Post -Uri "$api/telemetry/batch" -Headers $headers -Body $body
```

Expected: HTTP `200`.

## CloudWatch logs
- Open Lambda log group for ingest function.
- Confirm no auth/parse/write errors.

## Timestream sample query
```sql
SELECT *
FROM "<prefix>-chamber"."<prefix>-telemetry"
WHERE time > ago(1h)
LIMIT 20
```

## 5. Validate dashboard
- Open `http://<ec2-public-ip>:8501`
- Login with username/password from deploy prompts
- Confirm these views work:
  - latest panel
  - history chart
  - event timeline

## 6. Provision firmware with generated CFG commands
The script prints exact commands.
Example:

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

Open Wi-Fi example (no password):

```text
CFG WIFI_SSID <your_open_ssid>
CFG WIFI_PASS
```

On device:
- Open serial monitor at `115200`
- Send commands line-by-line
- In Service menu, open `WiFi Status`
- Verify HTTP code and sent counter increase over time
- If token was auto-generated, use exactly the printed `CFG API_TOKEN ...` value.

## 7. Expected runtime behavior (SD-first)
- If Wi-Fi fails: experiment continues and SD logs continue normally
- If cloud/API fails: retry/backoff occurs, no experiment stop
- On reconnect: pending unsynced records continue uploading
- SD files are not deleted by cloud uploader

## 8. Troubleshooting

## TLS / ESP-AT issues
- Check ESP8266 baud and AT firmware version
- Ensure API host/path/token are correct
- Try `CFG TEST` and inspect `WiFi Status`
- Open network with captive portal is not supported by this AT flow. Use open SSID without portal, or WPA/WPA2.

## Security group/IP issues
- If your public IP changes, rerun one-shot (or update SG rule)
- Confirm port 8501 is allowed only from your CIDR

## SAM deploy failures
- Check IAM permissions
- Retry `sam build` and `sam deploy` in `cloud/lambda_ingest`

## Timestream permission errors
- Confirm Lambda role includes `WriteRecords` + `DescribeEndpoints`
- Confirm EC2 role includes `Select` + `DescribeEndpoints`

## EC2 dashboard not reachable
- Check instance is running and status checks are OK
- SSH and run:
  - `sudo systemctl status chamber-streamlit.service`
  - `sudo journalctl -u chamber-streamlit.service -n 200`

## 9. Rollback

## Delete ingest stack
```powershell
aws cloudformation delete-stack --stack-name <prefix>-ingest --region us-east-1
```

## Terminate EC2 and clean SG/profile/role
- Terminate instance with tag `<prefix>-streamlit`
- Delete SG `<prefix>-streamlit-sg`
- Delete instance profile and role:
  - `<prefix>-streamlit-profile`
  - `<prefix>-streamlit-role`

## Optional Timestream cleanup
```powershell
aws timestream-write delete-table --database-name <prefix>-chamber --table-name <prefix>-telemetry --region us-east-1
aws timestream-write delete-table --database-name <prefix>-chamber --table-name <prefix>-events --region us-east-1
aws timestream-write delete-database --database-name <prefix>-chamber --region us-east-1
```
