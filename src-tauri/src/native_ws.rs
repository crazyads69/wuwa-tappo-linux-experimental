//! Native WebSocket client for the multiplayer socket (socket.io over wss).
//!
//! Kuro's socket server answers a WebSocket handshake with 403 unless the `Origin` header is absent or exactly
//! `http(s)://tauri.localhost`. WebKitGTK sends `tauri://localhost`, so the browser WebSocket can never connect.
//! `ws.js` redirects that one URL here. This client sends no `Origin` and no cookie (the same thing any non-browser
//! client does) and identifies itself honestly in `User-Agent`. It only ever connects to the one allowed
//! host and path, so a compromised page cannot use it as a general network proxy.
use base64::Engine as _;
use futures_util::{SinkExt, StreamExt};
use serde::Serialize;
use std::{collections::HashMap, sync::Mutex, time::Duration};
use tauri::{ipc::Channel, AppHandle, Manager, State};
use tokio::sync::mpsc;
use tokio_tungstenite::{
    connect_async_with_config,
    tungstenite::{
        self,
        client::IntoClientRequest,
        http::{header, HeaderValue, Request},
        protocol::{frame::coding::CloseCode, CloseFrame, WebSocketConfig},
        Message,
    },
};
use url::Url;

const HOST: &str = "api-wutheringwaves-wuwatappo.kurogames-global.com";
const MAX_SOCKETS: usize = 4;
const MAX_MESSAGE: usize = 4 << 20;
const QUEUE: usize = 256;
const CONNECT_TIMEOUT: Duration = Duration::from_secs(20);
const CLOSE_GRACE: Duration = Duration::from_secs(3);

pub enum Out {
    Text(String),
    Binary(Vec<u8>),
    Close(u16, String),
}

/// Sent to the page through a Tauri channel; `ws.js` turns them into WebSocket events.
#[derive(Serialize, Clone, Debug, PartialEq)]
#[serde(tag = "event", rename_all = "camelCase")]
pub enum Event {
    Open { protocol: String, extensions: String },
    Message { kind: &'static str, data: String },
    Close { code: u16, reason: String, clean: bool },
    Error { message: String },
}

struct Entry {
    label: String,
    tx: mpsc::Sender<Out>,
}

#[derive(Default)]
pub struct Sockets(Mutex<HashMap<String, Entry>>);

impl Sockets {
    /// Closing a window must not leave its sockets alive in the background.
    pub fn close_window(&self, label: &str) {
        for e in self.0.lock().unwrap().values().filter(|e| e.label == label) {
            let _ = e.tx.try_send(Out::Close(1001, String::new()));
        }
    }
}

fn validate_url(raw: &str) -> Result<Url, String> {
    let url = Url::parse(raw).map_err(|_| "invalid WebSocket URL")?;
    let ok = raw.len() <= 4096
        && url.scheme() == "wss"
        && url.host_str() == Some(HOST)
        && url.port().is_none() // 443 is the default and parses to None
        && matches!(url.path(), "/socket.io/" | "/socket.io")
        && url.username().is_empty()
        && url.password().is_none()
        && url.fragment().is_none();
    if ok { Ok(url) } else { Err("WebSocket destination is not allowed".into()) }
}

fn is_token(s: &str) -> bool {
    !s.is_empty() && s.bytes().all(|b| b.is_ascii_alphanumeric() || b"!#$%&'*+-.^_`|~".contains(&b))
}

fn request_for(url: &str, protocols: &[String]) -> Result<Request<()>, String> {
    let mut req = url.into_client_request().map_err(|e| e.to_string())?;
    if !protocols.is_empty() {
        if protocols.len() > 8 || !protocols.iter().all(|p| is_token(p)) {
            return Err("invalid WebSocket subprotocol".into());
        }
        let v = HeaderValue::from_str(&protocols.join(", ")).map_err(|e| e.to_string())?;
        req.headers_mut().insert(header::SEC_WEBSOCKET_PROTOCOL, v);
    }
    req.headers_mut().insert(header::USER_AGENT, HeaderValue::from_static(concat!("wuwatappo-linux/", env!("CARGO_PKG_VERSION"), " (unofficial)")));
    Ok(req)
}

fn safe_error(e: &tungstenite::Error) -> String {
    match e {
        tungstenite::Error::Http(r) => format!("WebSocket handshake rejected (HTTP {})", r.status().as_u16()),
        tungstenite::Error::Io(_) => "WebSocket network connection failed".into(),
        tungstenite::Error::Tls(_) => "WebSocket TLS connection failed".into(),
        _ => "WebSocket connection failed".into(),
    }
}

fn close_event(frame: Option<CloseFrame>, clean: bool) -> Event {
    match frame {
        Some(f) => Event::Close { code: f.code.into(), reason: f.reason.to_string(), clean },
        None => Event::Close { code: if clean { 1005 } else { 1006 }, reason: String::new(), clean },
    }
}

/// One connection from handshake to close. Independent of Tauri so it can be tested against a local server.
async fn run(request: Request<()>, mut rx: mpsc::Receiver<Out>, emit: impl Fn(Event)) {
    let cfg = WebSocketConfig::default().max_message_size(Some(MAX_MESSAGE)).max_frame_size(Some(MAX_MESSAGE));
    let connected = tokio::time::timeout(CONNECT_TIMEOUT, connect_async_with_config(request, Some(cfg), false)).await;
    let (stream, response) = match connected {
        Ok(Ok(ok)) => ok,
        other => {
            let message = match other {
                Ok(Err(e)) => safe_error(&e),
                _ => "WebSocket connection timed out".into(),
            };
            emit(Event::Error { message });
            emit(Event::Close { code: 1006, reason: String::new(), clean: false });
            return;
        }
    };
    let header_text = |name| response.headers().get(name).and_then(|v| v.to_str().ok()).unwrap_or("").to_string();
    emit(Event::Open { protocol: header_text(header::SEC_WEBSOCKET_PROTOCOL), extensions: header_text(header::SEC_WEBSOCKET_EXTENSIONS) });

    let (mut sink, mut incoming) = stream.split();
    let mut deadline: Option<tokio::time::Instant> = None; // set once we have sent our Close frame
    loop {
        let expire = async {
            match deadline {
                Some(d) => tokio::time::sleep_until(d).await,
                None => std::future::pending().await,
            }
        };
        tokio::select! {
            out = rx.recv(), if deadline.is_none() => match out {
                Some(Out::Text(t)) => if sink.send(Message::text(t)).await.is_err() { break },
                Some(Out::Binary(b)) => if sink.send(Message::binary(b)).await.is_err() { break },
                Some(Out::Close(code, reason)) => {
                    let frame = CloseFrame { code: CloseCode::from(code), reason: reason.into() };
                    if sink.send(Message::Close(Some(frame))).await.is_err() { break }
                    deadline = Some(tokio::time::Instant::now() + CLOSE_GRACE);
                }
                None => { let _ = sink.close().await; emit(close_event(None, true)); return; }
            },
            msg = incoming.next() => match msg {
                Some(Ok(Message::Text(t))) => emit(Event::Message { kind: "text", data: t.to_string() }),
                Some(Ok(Message::Binary(b))) => emit(Event::Message { kind: "binary", data: base64::engine::general_purpose::STANDARD.encode(&b) }),
                Some(Ok(Message::Close(frame))) => { emit(close_event(frame, true)); return; }
                Some(Ok(_)) => {} // ping/pong/frame: answered by the library
                Some(Err(e)) => { emit(Event::Error { message: safe_error(&e) }); break; }
                None => break,
            },
            _ = expire => break,
        }
    }
    emit(Event::Close { code: 1006, reason: String::new(), clean: false });
}

#[tauri::command]
pub fn native_ws_connect(window: tauri::WebviewWindow, state: State<Sockets>, id: String, url: String, protocols: Vec<String>, events: Channel<Event>) -> Result<(), String> {
    if id.is_empty() || id.len() > 64 || !id.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_') {
        return Err("invalid socket id".into());
    }
    let url = validate_url(&url)?;
    let request = request_for(url.as_str(), &protocols)?;
    let (tx, rx) = mpsc::channel(QUEUE);
    {
        let mut map = state.0.lock().unwrap();
        if map.len() >= MAX_SOCKETS || map.contains_key(&id) {
            return Err("too many WebSockets".into());
        }
        map.insert(id.clone(), Entry { label: window.label().to_string(), tx });
    }
    let app: AppHandle = window.app_handle().clone();
    tauri::async_runtime::spawn(async move {
        run(request, rx, |e| { let _ = events.send(e); }).await;
        app.state::<Sockets>().0.lock().unwrap().remove(&id);
    });
    Ok(())
}

fn sender(state: &Sockets, id: &str) -> Result<mpsc::Sender<Out>, String> {
    state.0.lock().unwrap().get(id).map(|e| e.tx.clone()).ok_or_else(|| "unknown WebSocket".to_string())
}

#[tauri::command]
pub fn native_ws_send(state: State<Sockets>, id: String, kind: String, data: String) -> Result<(), String> {
    let out = match kind.as_str() {
        "text" => Out::Text(data),
        "binary" => Out::Binary(base64::engine::general_purpose::STANDARD.decode(data).map_err(|_| "invalid binary payload")?),
        _ => return Err("invalid payload kind".into()),
    };
    sender(&state, &id)?.try_send(out).map_err(|_| "WebSocket is closed or backed up".to_string())
}

#[tauri::command]
pub fn native_ws_close(state: State<Sockets>, id: String, code: Option<u16>, reason: Option<String>) -> Result<(), String> {
    let code = code.unwrap_or(1000);
    let reason = reason.unwrap_or_default();
    if !(code == 1000 || (3000..=4999).contains(&code)) || reason.len() > 123 {
        return Err("invalid close code or reason".into());
    }
    sender(&state, &id)?.try_send(Out::Close(code, reason)).map_err(|_| "WebSocket is closed or backed up".to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Mutex as StdMutex};
    use tokio::net::TcpListener;

    #[test]
    fn only_the_exact_socket_endpoint_is_allowed() {
        let ok = |s: &str| validate_url(s).is_ok();
        assert!(ok(&format!("wss://{HOST}/socket.io/?EIO=3&transport=websocket&token=x")));
        assert!(ok(&format!("wss://{HOST}:443/socket.io/")));
        assert!(ok(&format!("wss://{HOST}/socket.io")));
        for bad in [
            format!("ws://{HOST}/socket.io/"),
            format!("https://{HOST}/socket.io/"),
            format!("wss://{HOST}:444/socket.io/"),
            format!("wss://{HOST}/other"),
            format!("wss://{HOST}/socket.io/x"),
            format!("wss://{HOST}.example/socket.io/"),
            format!("wss://user@{HOST}/socket.io/"),
            format!("wss://{HOST}/socket.io/#frag"),
            "wss://example.com/socket.io/".to_string(),
            "not a url".to_string(),
        ] {
            assert!(!ok(&bad), "{bad}");
        }
    }

    #[test]
    fn request_has_no_origin_or_cookie_and_checks_subprotocols() {
        let req = request_for(&format!("wss://{HOST}/socket.io/"), &["v1.test".into()]).unwrap();
        assert!(!req.headers().contains_key(header::ORIGIN));
        assert!(!req.headers().contains_key(header::COOKIE));
        assert_eq!(req.headers()[header::SEC_WEBSOCKET_PROTOCOL], "v1.test");
        assert!(req.headers()[header::USER_AGENT].to_str().unwrap().contains("unofficial"));
        assert!(request_for(&format!("wss://{HOST}/socket.io/"), &["bad proto".into()]).is_err());
        assert!(request_for(&format!("wss://{HOST}/socket.io/"), &vec!["a".to_string(); 9]).is_err());
    }

    /// Echo server on loopback that records the handshake headers it saw.
    async fn echo_server() -> (u16, Arc<StdMutex<Option<tungstenite::http::HeaderMap>>>) {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let port = listener.local_addr().unwrap().port();
        let seen = Arc::new(StdMutex::new(None));
        let record = seen.clone();
        tokio::spawn(async move {
            let (tcp, _) = listener.accept().await.unwrap();
            let mut ws = tokio_tungstenite::accept_hdr_async(tcp, move |req: &tungstenite::handshake::server::Request, resp| {
                *record.lock().unwrap() = Some(req.headers().clone());
                Ok(resp)
            })
            .await
            .unwrap();
            while let Some(Ok(msg)) = ws.next().await {
                match msg {
                    Message::Text(_) | Message::Binary(_) => ws.send(msg).await.unwrap(),
                    Message::Close(_) => {} // keep polling: the library sends the close reply on the next poll
                    _ => {}
                }
            }
        });
        (port, seen)
    }

    #[tokio::test(flavor = "current_thread")]
    async fn text_and_binary_round_trip_without_origin_then_close() {
        let (port, seen) = echo_server().await;
        let (tx, rx) = mpsc::channel(8);
        let events = Arc::new(StdMutex::new(Vec::new()));
        let sink = events.clone();
        let request = request_for(&format!("ws://127.0.0.1:{port}/socket.io/"), &[]).unwrap();
        let task = tokio::spawn(run(request, rx, move |e| sink.lock().unwrap().push(e)));

        tx.send(Out::Text("hello".into())).await.unwrap();
        tx.send(Out::Binary(vec![1, 2, 3])).await.unwrap();
        tx.send(Out::Close(1000, "bye".into())).await.unwrap();
        task.await.unwrap();

        let headers = seen.lock().unwrap().clone().unwrap();
        assert!(!headers.contains_key(header::ORIGIN) && !headers.contains_key(header::COOKIE));
        let ev = events.lock().unwrap().clone();
        assert_eq!(ev[0], Event::Open { protocol: String::new(), extensions: String::new() });
        assert!(ev.contains(&Event::Message { kind: "text", data: "hello".into() }));
        assert!(ev.contains(&Event::Message { kind: "binary", data: "AQID".into() }));
        assert!(matches!(ev.last(), Some(Event::Close { clean: true, .. })));
    }

    #[tokio::test(flavor = "current_thread")]
    async fn connection_refused_reports_error_then_close() {
        let port = { TcpListener::bind("127.0.0.1:0").await.unwrap().local_addr().unwrap().port() }; // freed again
        let (_tx, rx) = mpsc::channel(1);
        let events = Arc::new(StdMutex::new(Vec::new()));
        let sink = events.clone();
        run(request_for(&format!("ws://127.0.0.1:{port}/socket.io/"), &[]).unwrap(), rx, move |e| sink.lock().unwrap().push(e)).await;
        let ev = events.lock().unwrap().clone();
        assert!(matches!(ev[0], Event::Error { .. }));
        assert!(matches!(ev[1], Event::Close { clean: false, .. }));
    }
}
