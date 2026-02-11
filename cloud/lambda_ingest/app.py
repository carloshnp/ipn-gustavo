import base64
import json
import os
import time
from datetime import datetime, timezone
from decimal import Decimal
from typing import Any, Dict, List, Tuple

import boto3

STORAGE_BACKEND = os.getenv("STORAGE_BACKEND", "dynamodb").strip().lower()
API_TOKEN = os.getenv("API_TOKEN", "")

TS_DB = os.getenv("TS_DB", "chamber")
TS_TABLE_TELE = os.getenv("TS_TABLE_TELEMETRY", "telemetry")
TS_TABLE_EVT = os.getenv("TS_TABLE_EVENTS", "events")

DDB_TABLE_TELE = os.getenv("DDB_TABLE_TELEMETRY", "telemetry")
DDB_TABLE_EVT = os.getenv("DDB_TABLE_EVENTS", "events")

_ts_client = boto3.client("timestream-write")
_ddb = boto3.resource("dynamodb")

TTL_SECONDS = 365 * 24 * 60 * 60


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


def _to_decimal(v: Any, default: str = "0") -> Decimal:
    try:
        return Decimal(str(v))
    except Exception:
        return Decimal(default)


def _parse_rtc_iso_to_ms(rtc_iso: str) -> int:
    if not rtc_iso:
        return 0
    try:
        s = rtc_iso.strip()
        if s.endswith("Z"):
            s = s[:-1] + "+00:00"
        dt = datetime.fromisoformat(s)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        return int(dt.timestamp() * 1000)
    except Exception:
        return 0


def _ttl_from_ms(ms: int) -> int:
    base_s = int(ms / 1000) if ms > 0 else int(time.time())
    return base_s + TTL_SECONDS


def _telemetry_items_ddb(device_id: str, records: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    for r in records:
        ms = _to_int(r.get("ms"), 0)
        if ms <= 0:
            ms = _parse_rtc_iso_to_ms(str(r.get("rtc_iso", "")))
        if ms <= 0:
            continue

        run_file = str(r.get("run_file", "")) or "unknown"
        line_index = _to_int(r.get("line_index"), 0)
        step = str(r.get("step", "")) or "-"
        sk = f"{ms:013d}#{run_file}#{line_index:08d}"

        out.append(
            {
                "device_id": device_id,
                "sk": sk,
                "ts_ms": ms,
                "expires_at": _ttl_from_ms(ms),
                "run_file": run_file,
                "line_index": line_index,
                "step": step,
                "mask": _to_int(r.get("mask"), 0),
                "t1": _to_decimal(_to_float(r.get("t1"), 0.0)),
                "u1": _to_decimal(_to_float(r.get("u1"), 0.0)),
                "t2": _to_decimal(_to_float(r.get("t2"), 0.0)),
                "u2": _to_decimal(_to_float(r.get("u2"), 0.0)),
                "tavg": _to_decimal(_to_float(r.get("tavg"), 0.0)),
                "uavg": _to_decimal(_to_float(r.get("uavg"), 0.0)),
                "rtc_iso": str(r.get("rtc_iso", "")),
                "sd_state": str(r.get("sd_state", "unknown")),
                "rtc_state": str(r.get("rtc_state", "unknown")),
                "run_state": str(r.get("run_state", "unknown")),
            }
        )
    return out


def _event_items_ddb(device_id: str, records: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    for idx, r in enumerate(records):
        seq = _to_int(r.get("line_index"), 0)
        if seq <= 0:
            seq = idx + 1

        ms = _to_int(r.get("ms"), 0)
        if ms <= 0:
            ms = _parse_rtc_iso_to_ms(str(r.get("rtc_iso", "")))
        if ms <= 0:
            ms = _to_int(r.get("line_index"), 0) * 1000
        if ms <= 0:
            ms = _to_int(r.get("current_step"), 0) * 1000
        if ms <= 0:
            ms = seq * 1000

        run_file = str(r.get("run_file", "unknown")) or "unknown"
        event_type = str(r.get("event_type", "evt")) or "evt"
        sk = f"{ms:013d}#{run_file}#{event_type}#{seq:08d}"

        out.append(
            {
                "device_id": device_id,
                "sk": sk,
                "ts_ms": ms,
                "expires_at": _ttl_from_ms(ms),
                "run_file": run_file,
                "event_type": event_type,
                "screen": str(r.get("screen", "unknown")),
                "arg0": _to_int(r.get("arg0"), 0),
                "arg1": _to_int(r.get("arg1"), 0),
                "current_step": _to_int(r.get("current_step"), 0),
                "rtc_iso": str(r.get("rtc_iso", "")),
            }
        )
    return out


def _write_ddb(table_name: str, items: List[Dict[str, Any]]) -> int:
    if not items:
        return 0
    table = _ddb.Table(table_name)
    accepted = 0
    with table.batch_writer(overwrite_by_pkeys=["device_id", "sk"]) as batch:
        for item in items:
            batch.put_item(Item=item)
            accepted += 1
    return accepted


def _telemetry_records_ts(device_id: str, records: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
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


def _event_records_ts(device_id: str, records: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    for idx, r in enumerate(records):
        seq = _to_int(r.get("line_index"), idx + 1)
        ms = _to_int(r.get("ms"), 0)
        if ms <= 0:
            ms = seq * 1000
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
                "Time": str(ms),
                "TimeUnit": "MILLISECONDS",
                "Version": seq if seq > 0 else 1,
            }
        )
    return out


def _write_ts(table: str, records: List[Dict[str, Any]]) -> int:
    if not records:
        return 0
    accepted = 0
    for i in range(0, len(records), 100):
        chunk = records[i : i + 100]
        _ts_client.write_records(DatabaseName=TS_DB, TableName=table, Records=chunk)
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
            if STORAGE_BACKEND == "dynamodb":
                items = _telemetry_items_ddb(device_id, records)
                accepted = _write_ddb(DDB_TABLE_TELE, items)
                last_key = None
                if items:
                    lk = items[-1]
                    last_key = {
                        "device_id": lk.get("device_id"),
                        "run_file": lk.get("run_file"),
                        "step": lk.get("step"),
                        "time": lk.get("ts_ms"),
                    }
                return _response(200, {"accepted": accepted, "last_key": last_key})
            if STORAGE_BACKEND == "timestream":
                ts_records = _telemetry_records_ts(device_id, records)
                accepted = _write_ts(TS_TABLE_TELE, ts_records)
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
            return _response(500, {"error": f"unsupported storage backend: {STORAGE_BACKEND}"})

        if path.endswith("/events/batch"):
            if STORAGE_BACKEND == "dynamodb":
                items = _event_items_ddb(device_id, records)
                accepted = _write_ddb(DDB_TABLE_EVT, items)
                last_key = None
                if items:
                    lk = items[-1]
                    last_key = {
                        "device_id": lk.get("device_id"),
                        "run_file": lk.get("run_file"),
                        "event_type": lk.get("event_type"),
                        "time": lk.get("ts_ms"),
                    }
                return _response(200, {"accepted": accepted, "last_key": last_key})
            if STORAGE_BACKEND == "timestream":
                evt_records = _event_records_ts(device_id, records)
                accepted = _write_ts(TS_TABLE_EVT, evt_records)
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
            return _response(500, {"error": f"unsupported storage backend: {STORAGE_BACKEND}"})

        return _response(404, {"error": "unknown route"})
    except Exception as exc:
        return _response(500, {"error": str(exc)})
