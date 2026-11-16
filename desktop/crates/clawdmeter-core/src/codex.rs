//! OpenAI Codex usage limits, shown on the device's second page.
//!
//! `GET https://chatgpt.com/backend-api/wham/usage` with the ChatGPT sign-in
//! Codex CLI keeps in `$CODEX_HOME/auth.json` (default `~/.codex`) answers
//! with up to two rate-limit windows:
//!
//! ```text
//! {"plan_type": "pro",
//!  "rate_limit": {"allowed": true, "limit_reached": false,
//!    "primary_window":   {"used_percent": 66, "limit_window_seconds": 604800,
//!                         "reset_after_seconds": 375538, "reset_at": 1795205209},
//!    "secondary_window": null}}
//! ```
//!
//! Which window is which comes from its length, not its slot: a plan with
//! only a weekly limit reports the week as `primary_window`. A window of a
//! day or less is the short ("session") one, anything longer the week.
//!
//! Like [`crate::oauth`], the token is only read, never refreshed: Codex
//! owns it. An expired or refused token leaves the last reading in place
//! until Codex renews it. The call sends no message, so it uses no quota.

use std::path::{Path, PathBuf};

use chrono::{DateTime, TimeZone, Utc};
use serde_json::Value;

use crate::oauth::{user_agent, HTTP_TIMEOUT};
use crate::usage::{UsageError, Window};

/// The usage endpoint.
pub const USAGE_URL: &str = "https://chatgpt.com/backend-api/wham/usage";
/// How often Codex is polled (s). Its numbers move only while Codex runs.
pub const POLL_EVERY: i64 = 5 * 60;
/// A reading older than this isn't sent (s): the device drops the page.
pub const MAX_AGE: i64 = 60 * 60;
/// Windows up to this long are the short one (s).
const SHORT_WINDOW_MAX: i64 = 24 * 3600;

/// One Codex reading. Either window may be missing (a plan without it).
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct CodexReading {
    /// The short window (5 hours on the plans that have one).
    pub session: Option<Window>,
    /// The weekly window.
    pub week: Option<Window>,
    /// A limit is used up.
    pub limited: bool,
    /// `plan_type`, e.g. "pro" (shown in the app only).
    pub plan: Option<String>,
}

/// The Codex home: `$CODEX_HOME`, else `~/.codex`.
pub fn codex_home(home: &Path, env: Option<&str>) -> PathBuf {
    match env.map(str::trim).filter(|s| !s.is_empty()) {
        Some(d) => PathBuf::from(d),
        None => home.join(".codex"),
    }
}

/// The ChatGPT sign-in Codex stored.
#[derive(Clone, PartialEq, Eq)]
pub struct Token {
    /// The bearer token.
    pub access: String,
    /// The ChatGPT account (workspace) it acts for.
    pub account_id: Option<String>,
}

impl std::fmt::Debug for Token {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Token").field("access", &"<redacted>").field("account_id", &self.account_id).finish()
    }
}

/// Pull the token out of `auth.json`: `{"tokens": {"access_token",
/// "account_id"}}`. An API-key-only login has no usage windows: `None`.
pub fn parse_auth(text: &str) -> Option<Token> {
    let v: Value = serde_json::from_str(text.trim()).ok()?;
    let t = v.get("tokens")?;
    let access = t.get("access_token")?.as_str().filter(|s| !s.is_empty())?.to_owned();
    let account_id = t.get("account_id").and_then(Value::as_str).filter(|s| !s.is_empty()).map(str::to_owned);
    Some(Token { access, account_id })
}

/// Read the token from `<codex home>/auth.json`.
pub fn read_token(codex_home: &Path) -> Option<Token> {
    parse_auth(&std::fs::read_to_string(codex_home.join("auth.json")).ok()?)
}

/// Is Codex signed in with ChatGPT here?
pub fn signed_in(codex_home: &Path) -> bool {
    read_token(codex_home).is_some()
}

fn window(v: &Value, now: DateTime<Utc>) -> Option<(i64, Window)> {
    if v.is_null() {
        return None;
    }
    let pct = v.get("used_percent")?.as_f64()?;
    let len = v.get("limit_window_seconds").and_then(Value::as_i64).unwrap_or(0);
    let resets_at =
        v.get("reset_at").and_then(Value::as_i64).and_then(|t| Utc.timestamp_opt(t, 0).single()).or_else(|| {
            v.get("reset_after_seconds").and_then(Value::as_i64).map(|s| now + chrono::Duration::seconds(s))
        });
    Some((len, Window { pct: pct.round().clamp(0.0, 999.0) as u32, resets_at }))
}

/// Read a 200 body.
pub fn parse_usage_json(body: &str, now: DateTime<Utc>) -> Result<CodexReading, UsageError> {
    let v: Value = serde_json::from_str(body).map_err(|e| UsageError::BadOutput(format!("codex usage: {e}")))?;
    let rl = v.get("rate_limit").filter(|r| !r.is_null());
    let mut r = CodexReading {
        plan: v.get("plan_type").and_then(Value::as_str).map(str::to_owned),
        limited: rl.and_then(|r| r.get("limit_reached")).and_then(Value::as_bool).unwrap_or(false),
        ..Default::default()
    };
    for key in ["primary_window", "secondary_window"] {
        let Some((len, w)) = rl.and_then(|r| r.get(key)).and_then(|w| window(w, now)) else { continue };
        let slot = if len > 0 && len <= SHORT_WINDOW_MAX { &mut r.session } else { &mut r.week };
        if slot.is_none() {
            *slot = Some(w);
        }
    }
    if r.session.is_none() && r.week.is_none() {
        return Err(UsageError::NoUsageLines("codex usage: no windows".into()));
    }
    Ok(r)
}

/// What one endpoint answer means.
pub fn interpret(
    status: u16,
    retry_after: Option<&str>,
    body: &str,
    now: DateTime<Utc>,
) -> Result<CodexReading, UsageError> {
    match status {
        200..=299 => parse_usage_json(body, now),
        401 | 403 => Err(UsageError::TokenRejected),
        429 => Err(UsageError::Throttled {
            retry_after_secs: retry_after.and_then(|r| r.trim().parse::<i64>().ok()).filter(|s| *s > 0),
        }),
        s => Err(UsageError::BadOutput(format!("codex usage answered HTTP {s}"))),
    }
}

/// Call the endpoint (blocking).
pub fn fetch_usage(token: &Token, now: DateTime<Utc>) -> Result<CodexReading, UsageError> {
    let agent: ureq::Agent =
        ureq::Agent::config_builder().http_status_as_error(false).timeout_global(Some(HTTP_TIMEOUT)).build().into();
    let mut req = agent
        .get(USAGE_URL)
        .header("Authorization", &format!("Bearer {}", token.access))
        .header("User-Agent", &user_agent())
        .header("Accept", "application/json");
    if let Some(id) = &token.account_id {
        req = req.header("ChatGPT-Account-Id", id);
    }
    let mut resp = req.call().map_err(|e| UsageError::Offline(e.to_string()))?;
    let status = resp.status().as_u16();
    let retry_after = resp.headers().get("retry-after").and_then(|v| v.to_str().ok()).map(str::to_owned);
    let body = resp.body_mut().read_to_string().unwrap_or_default();
    interpret(status, retry_after.as_deref(), &body, now)
}

/// Poll Codex once. `None` when it isn't signed in with ChatGPT here.
pub fn poll(codex_home: &Path, now: DateTime<Utc>) -> Option<Result<CodexReading, UsageError>> {
    let token = read_token(codex_home)?;
    Some(fetch_usage(&token, now))
}

/// When to poll Codex, and what the device last should show.
#[derive(Debug, Clone, Default)]
pub struct CodexState {
    /// The latest good reading and when it was read (unix s).
    pub last: Option<(i64, CodexReading)>,
    /// When the last poll started.
    pub last_attempt: Option<i64>,
    /// No poll before this (a 429's `retry-after`, or a miss's pause).
    pub blocked_until: Option<i64>,
    /// Misses in a row.
    pub strikes: u32,
    /// The last miss, for the log and the app.
    pub last_error: Option<UsageError>,
}

impl CodexState {
    /// A poll is due at `now`.
    pub fn due(&self, now: i64) -> bool {
        if self.blocked_until.is_some_and(|b| now < b) {
            return false;
        }
        self.last_attempt.is_none_or(|t| now - t >= POLL_EVERY)
    }

    /// Record one poll's answer. A miss keeps the last reading (no data is
    /// never a zero) and pauses 10, 20, then 30 min; a 429 waits exactly its
    /// `retry-after` when it sends one.
    pub fn record(&mut self, now: i64, r: Result<CodexReading, UsageError>) {
        self.last_attempt = Some(now);
        match r {
            Ok(reading) => {
                self.last = Some((now, reading));
                self.strikes = 0;
                self.blocked_until = None;
                self.last_error = None;
            }
            Err(e) => {
                self.strikes = (self.strikes + 1).min(3);
                let wait = match &e {
                    UsageError::Throttled { retry_after_secs: Some(s) } => *s,
                    _ => 600 * i64::from(self.strikes),
                };
                self.blocked_until = Some(now + wait);
                self.last_error = Some(e);
            }
        }
    }

    /// Codex isn't signed in with ChatGPT here: nothing to show, and no
    /// call until the next regular poll.
    pub fn record_signed_out(&mut self, now: i64) {
        *self = CodexState { last_attempt: Some(now), ..Default::default() };
    }

    /// The reading to show at `now`: `None` once it is older than [`MAX_AGE`].
    pub fn current(&self, now: i64) -> Option<&CodexReading> {
        self.last.as_ref().filter(|(at, _)| now - at <= MAX_AGE).map(|(_, r)| r)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn at(s: &str) -> DateTime<Utc> {
        DateTime::parse_from_rfc3339(s).unwrap().with_timezone(&Utc)
    }

    #[test]
    fn auth_json() {
        let t = parse_auth(
            r#"{"auth_mode":"chatgpt","OPENAI_API_KEY":null,"tokens":{"id_token":"i","access_token":"eyJ.a","refresh_token":"r","account_id":"acc-1"},"last_refresh":"2026-11-01T00:00:00Z"}"#,
        )
        .unwrap();
        assert_eq!((t.access.as_str(), t.account_id.as_deref()), ("eyJ.a", Some("acc-1")));
        assert!(!format!("{t:?}").contains("eyJ"), "never logged");
        assert_eq!(parse_auth(r#"{"auth_mode":"apikey","OPENAI_API_KEY":"sk-x","tokens":null}"#), None);
        assert_eq!(parse_auth("nope"), None);
    }

    #[test]
    fn weekly_only_plan() {
        // A real answer (ids removed): the week is the primary window.
        let body = r#"{"plan_type":"pro","rate_limit":{"allowed":true,"limit_reached":false,
            "primary_window":{"used_percent":66,"limit_window_seconds":604800,"reset_after_seconds":375538,"reset_at":1795205209},
            "secondary_window":null},"code_review_rate_limit":null,"credits":{"has_credits":false}}"#;
        let r = parse_usage_json(body, at("2026-11-16T12:00:00Z")).unwrap();
        assert_eq!(r.session, None);
        let w = r.week.unwrap();
        assert_eq!((w.pct, w.resets_at.unwrap().timestamp()), (66, 1_795_205_209));
        assert_eq!(r.plan.as_deref(), Some("pro"));
        assert!(!r.limited);
    }

    #[test]
    fn five_hour_and_week() {
        let now = at("2026-11-16T12:00:00Z");
        let body = r#"{"rate_limit":{"limit_reached":true,
            "primary_window":{"used_percent":100.4,"limit_window_seconds":18000,"reset_after_seconds":600},
            "secondary_window":{"used_percent":12.6,"limit_window_seconds":604800,"reset_at":1795205209}}}"#;
        let r = parse_usage_json(body, now).unwrap();
        let s = r.session.unwrap();
        assert_eq!((s.pct, s.resets_at), (100, Some(now + chrono::Duration::seconds(600))));
        assert_eq!(r.week.unwrap().pct, 13);
        assert!(r.limited);
    }

    #[test]
    fn no_windows_is_a_miss() {
        let now = at("2026-11-16T12:00:00Z");
        assert!(parse_usage_json(r#"{"rate_limit":null}"#, now).is_err());
        assert!(matches!(interpret(401, None, "", now), Err(UsageError::TokenRejected)));
        assert!(matches!(
            interpret(429, Some("120"), "", now),
            Err(UsageError::Throttled { retry_after_secs: Some(120) })
        ));
    }

    #[test]
    fn state_backs_off_and_keeps_the_last_reading() {
        let mut st = CodexState::default();
        assert!(st.due(0));
        let good = CodexReading { week: Some(Window { pct: 5, resets_at: None }), ..Default::default() };
        st.record(0, Ok(good.clone()));
        assert!(!st.due(POLL_EVERY - 1));
        assert!(st.due(POLL_EVERY));
        st.record(POLL_EVERY, Err(UsageError::Offline("x".into())));
        assert_eq!(st.current(POLL_EVERY), Some(&good), "a miss is never a zero");
        assert!(!st.due(POLL_EVERY * 2), "paused 10 min after a miss");
        assert!(st.due(POLL_EVERY + 600));
        st.record(1000, Err(UsageError::Throttled { retry_after_secs: Some(42) }));
        assert_eq!(st.blocked_until, Some(1042));
        assert_eq!(st.current(MAX_AGE + 1), None, "too old to show");
    }

    #[test]
    fn home_dir() {
        assert_eq!(codex_home(Path::new("/h"), None), PathBuf::from("/h/.codex"));
        assert_eq!(codex_home(Path::new("/h"), Some("/x")), PathBuf::from("/x"));
        assert_eq!(codex_home(Path::new("/h"), Some(" ")), PathBuf::from("/h/.codex"));
    }
}
