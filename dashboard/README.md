# Streamlit Dashboard

## Run locally
```bash
python -m venv .venv
. .venv/Scripts/activate
pip install -r requirements.txt
set APP_USER=admin
set APP_PASSWORD_SHA256=<sha256_password>
set AWS_REGION=us-east-1
set STORAGE_BACKEND=dynamodb
set DDB_TABLE_TELEMETRY=telemetry
set DDB_TABLE_EVENTS=events
# Optional Timestream mode:
# set STORAGE_BACKEND=timestream
# set TS_DB=chamber
# set TS_TABLE_TELEMETRY=telemetry
# set TS_TABLE_EVENTS=events
streamlit run streamlit_app.py
```

## EC2 deployment notes
- DynamoDB mode: use IAM role with `dynamodb:Query`, `dynamodb:Scan`, `dynamodb:GetItem`, `dynamodb:BatchGetItem`.
- Optional Timestream mode: keep `timestream:Select` permissions.
- Put Streamlit behind Nginx reverse proxy.
- Run as a `systemd` service.
