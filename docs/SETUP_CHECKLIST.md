# End-to-End Setup Checklist (Arduino + GitHub + AWS + Streamlit)

Use this as a practical deployment runbook. Check each box as you complete the step.

## 1. Prepare Local Project and Secrets

- [ ] Confirm you are in the correct project folder:
  - `C:\Users\carlo\OneDrive\Documentos\PlatformIO\Projects\260122-133648-megaatmega2560`
- [ ] Move sensitive files out of the repo folder (or keep them ignored):
  - `*.pem`
  - `*credentials*.csv`
  - `*accessKeys*.csv`
- [ ] Verify `.gitignore` includes:
  - `.pio/`
  - `deploy/out/`
  - `.aws-sam/`
  - secret patterns (`*.pem`, credentials/access key files, `.env`)

## 2. Initialize Git and Push to GitHub

- [ ] Initialize git (if not initialized):
  - `git init`
- [ ] Create first commit:
  - `git add .`
  - `git commit -m "Initial project: Mega firmware + deploy + docs"`
- [ ] Create empty repo on GitHub.
- [ ] Connect local repo to GitHub:
  - `git branch -M main`
  - `git remote add origin https://github.com/<user>/<repo>.git`
  - `git push -u origin main`

## 3. Install and Verify Tooling

- [ ] Install/verify AWS CLI:
  - `aws --version`
- [ ] Install/verify SAM CLI:
  - `sam --version`
- [ ] Install/verify Python:
  - `python --version`
- [ ] Install/verify OpenSSH client:
  - `ssh -V`
- [ ] Install/verify PlatformIO CLI:
  - `platformio --version`

## 4. Configure AWS Access

- [ ] Create IAM user for deployment with required permissions.
- [ ] Create/download AWS access key for that user.
- [ ] Configure local CLI profile:
  - `aws configure`
  - Region: `us-east-1`
- [ ] Verify authentication:
  - `aws sts get-caller-identity`

## 5. Prepare EC2 Key Pair

- [ ] Confirm an EC2 key pair exists in `us-east-1`.
- [ ] Keep the `.pem` file secure (do not commit to GitHub).

## 6. Run One-Shot Cloud Deployment

- [ ] Open PowerShell at project root.
- [ ] Run:
  - `.\deploy\one_shot_deploy.ps1`
  - optional explicit backend: `.\deploy\one_shot_deploy.ps1 -StorageBackend dynamodb`
- [ ] Fill prompts:
  - project prefix
  - key pair name
  - device id
  - streamlit username
  - Wi-Fi SSID
  - Wi-Fi password (blank for open Wi-Fi)
  - API token (blank allowed, script auto-generates)
  - Streamlit password
- [ ] Save outputs:
  - API URL
  - EC2 public IP / dashboard URL
  - printed `CFG ...` commands
  - receipt file `deploy/out/<prefix>-deployment.json`

## 7. Build and Upload Firmware to Arduino Mega

- [ ] Confirm `platformio.ini` environment is correct (`[env:megaatmega2560]`).
- [ ] Build firmware:
  - `platformio run`
- [ ] Find serial port:
  - `platformio device list`
- [ ] Upload firmware:
  - `platformio run -t upload --upload-port COMx`

## 8. Provision Device Cloud Settings (Serial CFG)

- [ ] Open serial monitor:
  - `platformio device monitor --port COMx --baud 115200`
- [ ] Send commands printed by deployment script:
  - `CFG WIFI_SSID ...`
  - `CFG WIFI_PASS ...` (or blank command for open Wi-Fi)
  - `CFG API_HOST ...`
  - `CFG API_PATH /v1`
  - `CFG API_TOKEN ...`
  - `CFG DEVICE_ID ...`
  - `CFG WIFI_ENABLE 1`
  - `CFG SAVE`
  - `CFG TEST`
  - `CFG SHOW`

## 9. Validate End-to-End Operation

- [ ] Device local behavior:
  - SD logging continues normally (SD-first)
  - no experiment freeze if Wi-Fi is unstable
- [ ] Device network behavior:
  - `WiFi Status` reaches online state
  - upload counters increase
- [ ] AWS backend:
  - Lambda logs show successful requests
  - DynamoDB tables receive records
- [ ] Streamlit dashboard:
  - login works
  - live/historical charts show incoming data

## 10. Operations and Maintenance

- [ ] Rotate API token when needed:
  - update backend token
  - reprovision firmware token (`CFG API_TOKEN ...`, `CFG SAVE`, `CFG TEST`)
- [ ] Rotate Streamlit password when needed.
- [ ] Restrict Security Group ingress to your current IP/CIDR.
- [ ] Keep cloud monitoring non-critical; SD remains authoritative storage.

## Quick Safety Notes

- [ ] Never commit credentials, key files, or tokens to GitHub.
- [ ] Keep `ipn-gustavo-streamlit.pem` outside version control.
- [ ] Prefer generating a new API token per deployment environment.
