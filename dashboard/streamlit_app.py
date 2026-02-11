import hashlib
import os
from datetime import datetime, timedelta, timezone

import boto3
import pandas as pd
import streamlit as st
from boto3.dynamodb.conditions import Key

st.set_page_config(page_title="Chamber Monitor", layout="wide")

AWS_REGION = os.getenv("AWS_REGION", "us-east-1")
STORAGE_BACKEND = os.getenv("STORAGE_BACKEND", "dynamodb").strip().lower()

DDB_TABLE_TELE = os.getenv("DDB_TABLE_TELEMETRY", "telemetry")
DDB_TABLE_EVT = os.getenv("DDB_TABLE_EVENTS", "events")

TS_DB = os.getenv("TS_DB", "chamber")
TS_TABLE_TELE = os.getenv("TS_TABLE_TELEMETRY", "telemetry")
TS_TABLE_EVT = os.getenv("TS_TABLE_EVENTS", "events")

APP_USER = os.getenv("APP_USER", "admin")
APP_PASSWORD_SHA256 = os.getenv("APP_PASSWORD_SHA256", "")


def check_login() -> bool:
    st.sidebar.header("Login")
    user = st.sidebar.text_input("User")
    password = st.sidebar.text_input("Password", type="password")
    if st.sidebar.button("Sign in"):
        digest = hashlib.sha256(password.encode("utf-8")).hexdigest()
        if user == APP_USER and digest == APP_PASSWORD_SHA256:
            st.session_state["auth"] = True
        else:
            st.session_state["auth"] = False
            st.sidebar.error("Invalid credentials")
    return bool(st.session_state.get("auth", False))


def ts_query(sql: str) -> pd.DataFrame:
    c = boto3.client("timestream-query", region_name=AWS_REGION)
    res = c.query(QueryString=sql)
    cols = [col["Name"] for col in res.get("ColumnInfo", [])]
    rows = []
    for row in res.get("Rows", []):
        vals = []
        for d in row.get("Data", []):
            vals.append(d.get("ScalarValue", None))
        rows.append(vals)
    return pd.DataFrame(rows, columns=cols)


def ddb_table(name: str):
    return boto3.resource("dynamodb", region_name=AWS_REGION).Table(name)


def now_ms() -> int:
    return int(datetime.now(timezone.utc).timestamp() * 1000)


def sk_bounds(minutes: int):
    end = now_ms()
    start = end - int(minutes * 60 * 1000)
    return f"{start:013d}#", f"{end:013d}#\uffff"


def to_numeric_cols(df: pd.DataFrame, cols):
    for c in cols:
        if c in df.columns:
            df[c] = pd.to_numeric(df[c], errors="coerce")


def latest_panel_timestream() -> pd.DataFrame:
    q = f"""
    SELECT device_id, run_file, max(time) AS t, approx_percentile(tavg, 0.5) AS tavg, approx_percentile(uavg, 0.5) AS uavg
    FROM \"{TS_DB}\".\"{TS_TABLE_TELE}\"
    WHERE measure_name = 'telemetry'
      AND time > ago(1h)
    GROUP BY device_id, run_file
    ORDER BY t DESC
    LIMIT 50
    """
    return ts_query(q)


def history_panel_timestream(device_id: str, minutes: int) -> pd.DataFrame:
    q = f"""
    SELECT time, device_id, run_file, step, t1, t2, tavg, u1, u2, uavg, mask
    FROM \"{TS_DB}\".\"{TS_TABLE_TELE}\"
    WHERE measure_name = 'telemetry'
      AND device_id = '{device_id}'
      AND time > ago({minutes}m)
    ORDER BY time ASC
    """
    return ts_query(q)


def events_panel_timestream(device_id: str, minutes: int) -> pd.DataFrame:
    q = f"""
    SELECT time, device_id, run_file, event_type, screen, arg0, arg1, current_step
    FROM \"{TS_DB}\".\"{TS_TABLE_EVT}\"
    WHERE measure_name = 'event'
      AND device_id = '{device_id}'
      AND time > ago({minutes}m)
    ORDER BY time DESC
    LIMIT 200
    """
    return ts_query(q)


def scan_dynamo_recent(table_name: str, minutes: int) -> pd.DataFrame:
    table = ddb_table(table_name)
    cutoff = now_ms() - int(minutes * 60 * 1000)
    items = []
    kwargs = {}
    while True:
        res = table.scan(**kwargs)
        for it in res.get("Items", []):
            if int(it.get("ts_ms", 0)) >= cutoff:
                items.append(it)
        lek = res.get("LastEvaluatedKey")
        if not lek:
            break
        kwargs["ExclusiveStartKey"] = lek
    return pd.DataFrame(items)


def latest_panel_dynamodb() -> pd.DataFrame:
    df = scan_dynamo_recent(DDB_TABLE_TELE, 60)
    if df.empty:
        return df
    to_numeric_cols(df, ["ts_ms", "tavg", "uavg"])
    df["t"] = pd.to_datetime(df["ts_ms"], unit="ms", utc=True, errors="coerce")
    grouped = (
        df.groupby(["device_id", "run_file"], dropna=False)
        .agg(t=("t", "max"), tavg=("tavg", "median"), uavg=("uavg", "median"))
        .reset_index()
        .sort_values("t", ascending=False)
    )
    return grouped.head(50)


def history_panel_dynamodb(device_id: str, minutes: int) -> pd.DataFrame:
    table = ddb_table(DDB_TABLE_TELE)
    sk_start, sk_end = sk_bounds(minutes)
    items = []
    kwargs = {
        "KeyConditionExpression": Key("device_id").eq(device_id) & Key("sk").between(sk_start, sk_end),
        "ScanIndexForward": True,
    }
    while True:
        res = table.query(**kwargs)
        items.extend(res.get("Items", []))
        lek = res.get("LastEvaluatedKey")
        if not lek:
            break
        kwargs["ExclusiveStartKey"] = lek
    df = pd.DataFrame(items)
    if df.empty:
        return df
    to_numeric_cols(df, ["ts_ms", "t1", "t2", "tavg", "u1", "u2", "uavg", "mask"])
    df["time"] = pd.to_datetime(df["ts_ms"], unit="ms", utc=True, errors="coerce")
    cols = ["time", "device_id", "run_file", "step", "t1", "t2", "tavg", "u1", "u2", "uavg", "mask"]
    return df[[c for c in cols if c in df.columns]]


def events_panel_dynamodb(device_id: str, minutes: int) -> pd.DataFrame:
    table = ddb_table(DDB_TABLE_EVT)
    sk_start, sk_end = sk_bounds(minutes)
    items = []
    kwargs = {
        "KeyConditionExpression": Key("device_id").eq(device_id) & Key("sk").between(sk_start, sk_end),
        "ScanIndexForward": False,
        "Limit": 200,
    }
    while len(items) < 200:
        res = table.query(**kwargs)
        items.extend(res.get("Items", []))
        if len(items) >= 200:
            items = items[:200]
            break
        lek = res.get("LastEvaluatedKey")
        if not lek:
            break
        kwargs["ExclusiveStartKey"] = lek
    df = pd.DataFrame(items)
    if df.empty:
        return df
    to_numeric_cols(df, ["ts_ms", "arg0", "arg1", "current_step"])
    df["time"] = pd.to_datetime(df["ts_ms"], unit="ms", utc=True, errors="coerce")
    cols = ["time", "device_id", "run_file", "event_type", "screen", "arg0", "arg1", "current_step"]
    return df[[c for c in cols if c in df.columns]].sort_values("time", ascending=False)


def latest_panel() -> pd.DataFrame:
    if STORAGE_BACKEND == "timestream":
        return latest_panel_timestream()
    return latest_panel_dynamodb()


def history_panel(device_id: str, minutes: int) -> pd.DataFrame:
    if STORAGE_BACKEND == "timestream":
        return history_panel_timestream(device_id, minutes)
    return history_panel_dynamodb(device_id, minutes)


def events_panel(device_id: str, minutes: int) -> pd.DataFrame:
    if STORAGE_BACKEND == "timestream":
        return events_panel_timestream(device_id, minutes)
    return events_panel_dynamodb(device_id, minutes)


st.title("Chamber SD-First Monitor")
st.caption("SD is primary storage. Cloud is auxiliary streaming.")

if not APP_PASSWORD_SHA256:
    st.error("Set APP_PASSWORD_SHA256 environment variable.")
    st.stop()

if not check_login():
    st.info("Sign in from sidebar.")
    st.stop()

st.sidebar.info(f"Backend: {STORAGE_BACKEND}")

colA, colB, colC = st.columns(3)
with colA:
    AWS_REGION = st.text_input("AWS Region", AWS_REGION)
with colB:
    if STORAGE_BACKEND == "timestream":
        TS_DB = st.text_input("Timestream DB", TS_DB)
    else:
        DDB_TABLE_TELE = st.text_input("DynamoDB telemetry table", DDB_TABLE_TELE)
with colC:
    if STORAGE_BACKEND == "timestream":
        TS_TABLE_TELE = st.text_input("Telemetry table", TS_TABLE_TELE)
    else:
        DDB_TABLE_EVT = st.text_input("DynamoDB events table", DDB_TABLE_EVT)

if STORAGE_BACKEND == "timestream":
    TS_TABLE_EVT = st.text_input("Events table", TS_TABLE_EVT)

refresh_s = st.slider("Auto refresh (s)", 3, 60, 5)
minutes = st.slider("History window (minutes)", 10, 1440, 120)
if st.button("Refresh now"):
    st.rerun()

latest_df = latest_panel()
st.subheader("Latest by Device/Run")
st.dataframe(latest_df, use_container_width=True)

if latest_df.empty:
    st.warning("No telemetry found.")
    st.stop()

devices = sorted(latest_df["device_id"].dropna().astype(str).unique().tolist())
sel = st.selectbox("Device", devices, index=0)

hist_df = history_panel(sel, minutes)
st.subheader("Temperature / Humidity")
if not hist_df.empty:
    if "time" in hist_df.columns:
        hist_df["time"] = pd.to_datetime(hist_df["time"], utc=True, errors="coerce")
    to_numeric_cols(hist_df, ["t1", "t2", "tavg", "u1", "u2", "uavg"])
    if "time" in hist_df.columns:
        st.line_chart(hist_df.set_index("time")[[c for c in ["t1", "t2", "tavg"] if c in hist_df.columns]], height=220)
        st.line_chart(hist_df.set_index("time")[[c for c in ["u1", "u2", "uavg"] if c in hist_df.columns]], height=220)
    st.dataframe(hist_df.tail(200), use_container_width=True)
else:
    st.info("No history rows for selected window")

st.subheader("Event Timeline")
evt_df = events_panel(sel, minutes)
st.dataframe(evt_df, use_container_width=True)

st.caption(f"Updated: {datetime.now(timezone.utc).isoformat()} | Suggested refresh: {refresh_s}s")
