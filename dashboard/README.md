# Streamlit Dashboard

## Run locally
```bash
python -m venv .venv
. .venv/Scripts/activate
pip install -r requirements.txt
set APP_USER=admin
set APP_PASSWORD_SHA256=<sha256_password>
set AWS_REGION=us-east-1
set TS_DB=chamber
set TS_TABLE_TELEMETRY=telemetry
set TS_TABLE_EVENTS=events
streamlit run streamlit_app.py
```

## EC2 deployment notes
- Use a dedicated IAM role with `timestream:Select` permissions.
- Put Streamlit behind Nginx reverse proxy.
- Run as a `systemd` service.
