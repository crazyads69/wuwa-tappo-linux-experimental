//! The recovered frontend forwards HTTP 601 here; its update route renders the maintenance notice.
use serde_json::{Map, Value};
use std::sync::Mutex;
use tauri::{AppHandle, Emitter, Manager, WebviewWindow};

#[derive(Default)]
pub struct ServiceStatus {
    // None means normal operation; Some(Null) means maintenance with the bundled default text.
    maintenance: Mutex<Option<Value>>,
}

fn notice_payload(payload: Value) -> Result<Value, String> {
    if payload.is_null() { return Ok(Value::Null); }
    let source = payload.as_object().ok_or("maintenance payload must be an object or null")?;
    let mut result = Map::new();
    for key in ["titleKey", "contentKey"] {
        if let Some(value) = source.get(key) {
            let text = value.as_str().ok_or("maintenance dictionary key must be text")?.trim();
            if text.is_empty() || text.len() > 128 || !text.bytes().all(|c| c.is_ascii_alphanumeric() || b"_.-".contains(&c)) {
                return Err("invalid maintenance dictionary key".into());
            }
            result.insert(key.into(), Value::String(text.into()));
        }
    }
    for key in ["titleGroup", "contentGroup"] {
        if let Some(value) = source.get(key) {
            if value != "web" { return Err("invalid maintenance dictionary group".into()); }
            result.insert(key.into(), value.clone());
        }
    }
    if let Some(value) = source.get("assetsHost") {
        let text = value.as_str().ok_or("maintenance assets host must be text")?;
        let url = url::Url::parse(text).map_err(|_| "invalid maintenance assets host")?;
        if text.len() > 2048 || !matches!(url.scheme(), "https" | "http") || url.host_str().is_none()
            || !url.username().is_empty() || url.password().is_some() || url.query().is_some() || url.fragment().is_some() {
            return Err("invalid maintenance assets host".into());
        }
        result.insert("assetsHost".into(), Value::String(url.as_str().trim_end_matches('/').into()));
    }
    Ok(Value::Object(result))
}

#[tauri::command]
pub fn open_maintenance_notice(app: AppHandle, state: tauri::State<ServiceStatus>, payload: Option<Value>) -> super::R {
    let payload = notice_payload(payload.unwrap_or(Value::Null))?;
    let (window, created) = super::aux(&app, "update", "/update", "Service maintenance", 580.0, 600.0, |b| b)?;
    *state.maintenance.lock().unwrap() = Some(payload.clone());
    app.emit("cat-force-update-lock", true).map_err(super::e2s)?;
    super::present(&app, &window, created, move |app, _| {
        super::emit_later(app, "update", "maintenance-notice-open", payload);
    });
    Ok(())
}

#[tauri::command]
pub fn has_maintenance_notice(state: tauri::State<ServiceStatus>) -> bool {
    state.maintenance.lock().unwrap().is_some()
}

#[tauri::command]
pub fn get_maintenance_notice_payload(state: tauri::State<ServiceStatus>) -> Value {
    state.maintenance.lock().unwrap().clone().unwrap_or(Value::Null)
}

#[tauri::command]
pub fn is_force_update_locked(state: tauri::State<ServiceStatus>) -> bool {
    state.maintenance.lock().unwrap().is_some()
}

#[tauri::command]
pub fn hide_main_after_maintenance_notice_ready(app: AppHandle, window: WebviewWindow, state: tauri::State<ServiceStatus>) -> super::R {
    if window.label() != "update" || !state.maintenance.lock().unwrap().is_some() {
        return Err("maintenance notice is not active in this window".into());
    }
    if let Some(main) = app.get_webview_window("main") { main.hide().map_err(super::e2s)?; }
    Ok(())
}

// The official feed provides Windows installers, not signed releases of this independent Linux port.
#[tauri::command]
pub fn check_app_update(endpoint: Option<String>) -> Result<Option<Value>, String> {
    let _ = endpoint;
    Err("No signed Linux update feed is configured. Windows installers cannot update this Linux port.".into())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn maintenance_default_is_active_without_remote_text() {
        let state = ServiceStatus::default();
        assert!(state.maintenance.lock().unwrap().is_none());
        *state.maintenance.lock().unwrap() = Some(notice_payload(Value::Null).unwrap());
        assert!(state.maintenance.lock().unwrap().is_some());
        assert_eq!(state.maintenance.lock().unwrap().as_ref(), Some(&Value::Null));
    }

    #[test]
    fn accepts_the_recovered_http_601_payload_and_drops_unknown_fields() {
        assert_eq!(notice_payload(json!({"assetsHost":"https://assets.example/app/","titleGroup":"web","titleKey":"maintenance.title","contentGroup":"web","contentKey":"maintenance_body-1","ignored":"unused"})).unwrap(),
            json!({"assetsHost":"https://assets.example/app","titleGroup":"web","titleKey":"maintenance.title","contentGroup":"web","contentKey":"maintenance_body-1"}));
        assert_eq!(notice_payload(json!({})).unwrap(), json!({}));
    }

    #[test]
    fn rejects_invalid_notice_fields_and_non_web_assets() {
        for bad in [json!([]), json!("text"), json!({"titleKey":"../bad key"}), json!({"titleKey":""}),
            json!({"contentKey":"a".repeat(129)}), json!({"contentGroup":"other"}),
            json!({"assetsHost":"file:///etc/passwd"}), json!({"assetsHost":"javascript:alert(1)"}),
            json!({"assetsHost":"https://user:secret@example.com"}), json!({"assetsHost":"https://example.com/?token=secret"})] {
            assert!(notice_payload(bad).is_err());
        }
    }

    #[test]
    fn windows_update_is_an_explicit_error_not_up_to_date_or_an_installer() {
        let error = check_app_update(Some("https://example.com/windows.exe".into())).unwrap_err();
        assert!(error.contains("Linux"));
        assert!(error.contains("Windows installers cannot"));
    }
}
