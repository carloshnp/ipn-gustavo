# Lambda Ingest (API Gateway -> DynamoDB/Timestream)

## Routes
- `POST /v1/telemetry/batch`
- `POST /v1/events/batch`

## Required headers
- `X-Device-Id`
- `X-Api-Token`
- `Content-Type: application/json`

## Environment variables
- `API_TOKEN`
- `STORAGE_BACKEND` (`dynamodb` or `timestream`)
- `DDB_TABLE_TELEMETRY`
- `DDB_TABLE_EVENTS`
- `TS_DB`
- `TS_TABLE_TELEMETRY`
- `TS_TABLE_EVENTS`

## Deploy (SAM)
```bash
sam build
sam deploy --guided
```

## Notes
- DynamoDB path is idempotent by deterministic key (`device_id` + `sk`).
- Timestream path keeps record-version semantics.
- Keep API token private and rotate periodically.
