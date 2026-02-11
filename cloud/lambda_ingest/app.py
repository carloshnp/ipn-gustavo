import base64
import json
import os
from typing import Any, Dict, List, Tuple

import boto3

TS_DB = os.getenv("TS_DB", "chamber")
TS_TABLE_TELE = os.getenv("TS_TABLE_TELEMETRY", "telemetry")
TS_TABLE_EVT = os.getenv("TS_TABLE_EVENTS", "events")
API_TOKEN = os.getenv("API_TOKEN", "")

client = boto3.client("timestream-write")


def _response(code: int, payload: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "statusCode": code,
        "headers": {"Content-Type": "application/json"},
        "body": json.dumps(payload),
    }


def _parse_body(event: Dict[str, Any]) -> Dict[str, Any]:
    body = event.get("body") or "{}"
    if event.get("isBase64Encoded"):
        body = base64.b64decode(body).decode("utf-8")
    return json.loads(body)


def _headers_lower(event: Dict[str, Any]) -> Dict[str, str]:
    h = event.get("headers") or {}
    return {str(k).lower(): str(v) for k, v in h.items()}


def _auth_ok(event: Dict[str, Any], body: Dict[str, Any]) -> Tuple[bool, str]:
    h = _headers_lower(event)
    token = h.get("x-api-token", "")
    if not API_TOKEN:
        return False, "server token not configured"
    if token != API_TOKEN:
        return False, "invalid token"
    device = h.get("x-device-id", "") or str(body.get("device_id", ""))
    if not device:
        return False, "missing device id"
    return True, device


def _to_float(v: Any, default: float = 0.0) -> float:
    try:
        return float(v)
    except Exception:
        return default


def _to_int(v: Any, default: int = 0) -> int:
    try:
        return int(float(v))
    except Exception:
        return default


def _telemetry_records(device_id: str, records: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    for r in records:
        ms = _to_int(r.get("ms"), 0)
        if ms <= 0:
            continue
        run_file = str(r.get("run_file", ""))
        line_index = _to_int(r.get("line_index"), 0)
        step = str(r.get("step", ""))
        dims = [
            {"Name": "device_id", "Value": device_id},
            {"Name": "run_file", "Value": run_file or "unknown"},
            {"Name": "step", "Value": step or "-"},
            {"Name": "sd_state", "Value": str(r.get("sd_state", "unknown"))},
            {"Name": "rtc_state", "Value": str(r.get("rtc_state", "unknown"))},
            {"Name": "run_state", "Value": str(r.get("run_state", "unknown"))},
        ]
        mv = [
            {"Name": "line_index", "Value": str(line_index), "Type": "BIGINT"},
            {"Name": "mask", "Value": str(_to_int(r.get("mask"), 0)), "Type": "BIGINT"},
            {"Name": "t1", "Value": str(_to_float(r.get("t1"), 0.0)), "Type": "DOUBLE"},
            {"Name": "u1", "Value": str(_to_float(r.get("u1"), 0.0)), "Type": "DOUBLE"},
            {"Name": "t2", "Value": str(_to_float(r.get("t2"), 0.0)), "Type": "DOUBLE"},
            {"Name": "u2", "Value": str(_to_float(r.get("u2"), 0.0)), "Type": "DOUBLE"},
            {"Name": "tavg", "Value": str(_to_float(r.get("tavg"), 0.0)), "Type": "DOUBLE"},
            {"Name": "uavg", "Value": str(_to_float(r.get("uavg"), 0.0)), "Type": "DOUBLE"},
            {"Name": "rtc_iso", "Value": str(r.get("rtc_iso", "")), "Type": "VARCHAR"},
        ]
        out.append(
            {
                "Dimensions": dims,
                "MeasureName": "telemetry",
                "MeasureValueType": "MULTI",
                "MeasureValues": mv,
                "Time": str(ms),
                "TimeUnit": "MILLISECONDS",
                "Version": line_index if line_index > 0 else 1,
            }
        )
    return out


def _event_records(device_id: str, records: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    for r in records:
        ms = _to_int(r.get("line_index"), 0)
        if ms <= 0:
            ms = _to_int(r.get("current_step"), 0) + 1
        dims = [
            {"Name": "device_id", "Value": device_id},
            {"Name": "run_file", "Value": str(r.get("run_file", "unknown"))},
            {"Name": "event_type", "Value": str(r.get("event_type", "evt"))},
            {"Name": "screen", "Value": str(r.get("screen", "unknown"))},
        ]
        mv = [
            {"Name": "arg0", "Value": str(_to_int(r.get("arg0"), 0)), "Type": "BIGINT"},
            {"Name": "arg1", "Value": str(_to_int(r.get("arg1"), 0)), "Type": "BIGINT"},
            {"Name": "current_step", "Value": str(_to_int(r.get("current_step"), 0)), "Type": "BIGINT"},
            {"Name": "rtc_iso", "Value": str(r.get("rtc_iso", "")), "Type": "VARCHAR"},
        ]
        out.append(
            {
                "Dimensions": dims,
                "MeasureName": "event",
                "MeasureValueType": "MULTI",
                "MeasureValues": mv,
                "Time": str(_to_int(r.get("line_index"), 1) * 1000),
                "TimeUnit": "MILLISECONDS",
                "Version": _to_int(r.get("line_index"), 1),
            }
        )
    return out


def _write_records(table: str, records: List[Dict[str, Any]]) -> int:
    if not records:
        return 0
    # API max is 100 records per call
    accepted = 0
    for i in range(0, len(records), 100):
        chunk = records[i : i + 100]
        client.write_records(DatabaseName=TS_DB, TableName=table, Records=chunk)
        accepted += len(chunk)
    return accepted


def lambda_handler(event: Dict[str, Any], context: Any) -> Dict[str, Any]:
    try:
        body = _parse_body(event)
    except Exception as exc:
        return _response(400, {"error": f"invalid json: {exc}"})

    ok, auth_info = _auth_ok(event, body)
    if not ok:
        return _response(401, {"error": auth_info})
    device_id = auth_info

    path = (event.get("rawPath") or event.get("path") or "").rstrip("/")
    records = body.get("records") or []
    if not isinstance(records, list):
        return _response(400, {"error": "records must be list"})

    try:
        if path.endswith("/telemetry/batch"):
            ts_records = _telemetry_records(device_id, records)
            accepted = _write_records(TS_TABLE_TELE, ts_records)
            last_key = None
            if ts_records:
                lk = ts_records[-1]
                dims = {d["Name"]: d["Value"] for d in lk.get("Dimensions", [])}
                last_key = {
                    "device_id": dims.get("device_id"),
                    "run_file": dims.get("run_file"),
                    "step": dims.get("step"),
                    "time": lk.get("Time"),
                }
            return _response(200, {"accepted": accepted, "last_key": last_key})

        if path.endswith("/events/batch"):
            evt_records = _event_records(device_id, records)
            accepted = _write_records(TS_TABLE_EVT, evt_records)
            last_key = None
            if evt_records:
                lk = evt_records[-1]
                dims = {d["Name"]: d["Value"] for d in lk.get("Dimensions", [])}
                last_key = {
                    "device_id": dims.get("device_id"),
                    "run_file": dims.get("run_file"),
                    "event_type": dims.get("event_type"),
                    "time": lk.get("Time"),
                }
            return _response(200, {"accepted": accepted, "last_key": last_key})

        return _response(404, {"error": "unknown route"})
    except Exception as exc:
        return _response(500, {"error": str(exc)})
