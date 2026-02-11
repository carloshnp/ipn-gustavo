# Lambda Ingest (API Gateway -> Timestream)

## Routes
- `POST /v1/telemetry/batch`
- `POST /v1/events/batch`

## Required headers
- `X-Device-Id`
- `X-Api-Token`
- `Content-Type: application/json`

## Environment variables
- `API_TOKEN`
- `TS_DB`
- `TS_TABLE_TELEMETRY`
- `TS_TABLE_EVENTS`

## Deploy (SAM)
```bash
sam build
sam deploy --guided
```

## Notes
- This handler treats duplicates as idempotent by writing with `Version=line_index`.
- Keep API token private and rotate periodically.
