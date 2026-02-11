import hashlib
import os
from datetime import datetime, timedelta, timezone

import boto3
import pandas as pd
import streamlit as st

st.set_page_config(page_title="Chamber Monitor", layout="wide")

AWS_REGION = os.getenv("AWS_REGION", "us-east-1")
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
    cols = [col["Name"] for col in res["ColumnInfo"]]
    rows = []
    for row in res["Rows"]:
        vals = []
        for d in row["Data"]:
            vals.append(d.get("ScalarValue", None))
        rows.append(vals)
    return pd.DataFrame(rows, columns=cols)


def latest_panel() -> pd.DataFrame:
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


def history_panel(device_id: str, minutes: int) -> pd.DataFrame:
    q = f"""
    SELECT time, device_id, run_file, step, t1, t2, tavg, u1, u2, uavg, mask
    FROM \"{TS_DB}\".\"{TS_TABLE_TELE}\"
    WHERE measure_name = 'telemetry'
      AND device_id = '{device_id}'
      AND time > ago({minutes}m)
    ORDER BY time ASC
    """
    return ts_query(q)


def events_panel(device_id: str, minutes: int) -> pd.DataFrame:
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


st.title("Chamber SD-First Monitor")
st.caption("SD is primary storage. Cloud is auxiliary streaming.")

if not APP_PASSWORD_SHA256:
    st.error("Set APP_PASSWORD_SHA256 environment variable.")
    st.stop()

if not check_login():
    st.info("Sign in from sidebar.")
    st.stop()

colA, colB, colC = st.columns(3)
with colA:
    AWS_REGION = st.text_input("AWS Region", AWS_REGION)
with colB:
    TS_DB = st.text_input("Timestream DB", TS_DB)
with colC:
    TS_TABLE_TELE = st.text_input("Telemetry table", TS_TABLE_TELE)

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

devices = sorted(latest_df["device_id"].dropna().unique().tolist())
sel = st.selectbox("Device", devices, index=0)

hist_df = history_panel(sel, minutes)
st.subheader("Temperature / Humidity")
if not hist_df.empty:
    hist_df["time"] = pd.to_datetime(hist_df["time"], utc=True, errors="coerce")
    for c in ["t1", "t2", "tavg", "u1", "u2", "uavg"]:
        if c in hist_df.columns:
            hist_df[c] = pd.to_numeric(hist_df[c], errors="coerce")
    st.line_chart(hist_df.set_index("time")[[c for c in ["t1", "t2", "tavg"] if c in hist_df.columns]], height=220)
    st.line_chart(hist_df.set_index("time")[[c for c in ["u1", "u2", "uavg"] if c in hist_df.columns]], height=220)
    st.dataframe(hist_df.tail(200), use_container_width=True)
else:
    st.info("No history rows for selected window")

st.subheader("Event Timeline")
evt_df = events_panel(sel, minutes)
st.dataframe(evt_df, use_container_width=True)

st.caption(f"Updated: {datetime.now(timezone.utc).isoformat()} | Suggested refresh: {refresh_s}s")
