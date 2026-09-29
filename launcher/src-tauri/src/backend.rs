// Runs launcher/backend/server.mjs with Node and relays requests, responses and
// events over its stdin/stdout (one JSON message per line).
use std::collections::HashMap;
use std::fs::File;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use serde_json::{json, Value};
use tauri::{AppHandle, Emitter};
use tokio::sync::oneshot;

use crate::secret;

type Reply = Result<Value, String>;

pub struct Backend {
    stdin: Mutex<Option<ChildStdin>>,
    child: Mutex<Option<Child>>,
    pending: Arc<Mutex<HashMap<u64, oneshot::Sender<Reply>>>>,
    next: AtomicU64,
}

/// The launcher folder holding backend/ and core/. Refract runs from the repository,
/// so look beside and above the executable, then fall back to the source checkout.
pub fn launcher_dir() -> PathBuf {
    if let Some(dir) = std::env::var_os("REFRACT_LAUNCHER_DIR") {
        return PathBuf::from(dir);
    }
    if let Ok(exe) = std::env::current_exe() {
        for dir in exe.ancestors().skip(1) {
            if dir.join("backend/server.mjs").is_file() {
                return dir.to_path_buf();
            }
            if dir.join("launcher/backend/server.mjs").is_file() {
                return dir.join("launcher");
            }
        }
    }
    Path::new(env!("CARGO_MANIFEST_DIR")).join("..")
}

impl Backend {
    pub fn start(app: AppHandle, data_dir: PathBuf) -> Result<Self, String> {
        std::fs::create_dir_all(&data_dir).map_err(|e| format!("Could not create {}: {e}", data_dir.display()))?;
        let server = launcher_dir().join("backend/server.mjs");
        let log = File::create(data_dir.join("launcher-backend.log")).map_err(|e| e.to_string())?;
        let mut command = Command::new("node");
        command
            .arg(&server)
            .arg("--data")
            .arg(&data_dir)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(log);
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            const CREATE_NO_WINDOW: u32 = 0x0800_0000;
            command.creation_flags(CREATE_NO_WINDOW);
        }
        let mut child = command
            .spawn()
            .map_err(|e| format!("Could not start Node.js ({e}). Install Node.js 22 or later and try again."))?;
        let stdin = child.stdin.take();
        let stdout = child.stdout.take().ok_or("Backend has no output")?;
        let pending: Arc<Mutex<HashMap<u64, oneshot::Sender<Reply>>>> = Arc::default();

        let reader_pending = pending.clone();
        let token = secret::load(&data_dir);
        std::thread::spawn(move || {
            for line in BufReader::new(stdout).lines() {
                let Ok(line) = line else { break };
                let Ok(message) = serde_json::from_str::<Value>(&line) else { continue };
                if let Some(id) = message.get("id").and_then(Value::as_u64) {
                    let reply = if message.get("ok").and_then(Value::as_bool) == Some(true) {
                        Ok(message.get("value").cloned().unwrap_or(Value::Null))
                    } else {
                        Err(message.get("error").and_then(Value::as_str).unwrap_or("Launcher request failed.").to_string())
                    };
                    if let Some(sender) = reader_pending.lock().unwrap().remove(&id) {
                        let _ = sender.send(reply);
                    }
                    continue;
                }
                match message.get("event").and_then(Value::as_str) {
                    Some("changed") => { let _ = app.emit("refract://changed", message.get("payload")); }
                    Some("launch-error") => { let _ = app.emit("refract://launch-error", message.get("payload")); }
                    Some("secret") => {
                        if let Err(error) = secret::store(&data_dir, message.get("value").and_then(Value::as_str)) {
                            let _ = app.emit("refract://launch-error", format!("Meta sign-in could not be saved: {error}"));
                        }
                    }
                    _ => {}
                }
            }
            for (_, sender) in reader_pending.lock().unwrap().drain() {
                let _ = sender.send(Err("The launcher backend stopped. Restart Refract.".into()));
            }
            let _ = app.emit("refract://backend-exit", ());
        });

        let backend = Self { stdin: Mutex::new(stdin), child: Mutex::new(Some(child)), pending, next: AtomicU64::new(1) };
        if let Some(token) = token {
            backend.write(&json!({ "type": "secret", "value": token }))?;
        }
        Ok(backend)
    }

    fn write(&self, message: &Value) -> Result<(), String> {
        let mut guard = self.stdin.lock().unwrap();
        let stdin = guard.as_mut().ok_or("The launcher backend is not running.")?;
        writeln!(stdin, "{message}").and_then(|_| stdin.flush()).map_err(|_| "The launcher backend stopped. Restart Refract.".to_string())
    }

    pub async fn request(&self, method: &str, args: Vec<Value>) -> Reply {
        let id = self.next.fetch_add(1, Ordering::Relaxed);
        let (sender, receiver) = oneshot::channel();
        self.pending.lock().unwrap().insert(id, sender);
        if let Err(error) = self.write(&json!({ "id": id, "method": method, "args": args })) {
            self.pending.lock().unwrap().remove(&id);
            return Err(error);
        }
        receiver.await.unwrap_or_else(|_| Err("The launcher backend stopped. Restart Refract.".into()))
    }

    /// Closing stdin lets the backend cancel downloads and finish saving the library.
    /// A running game is a separate process and keeps going.
    pub fn shutdown(&self) {
        self.stdin.lock().unwrap().take();
        if let Some(mut child) = self.child.lock().unwrap().take() {
            std::thread::spawn(move || {
                for _ in 0..50 {
                    if matches!(child.try_wait(), Ok(Some(_))) { return; }
                    std::thread::sleep(std::time::Duration::from_millis(100));
                }
                let _ = child.kill();
            });
        }
    }
}
