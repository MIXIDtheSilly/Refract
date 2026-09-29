#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod backend;
mod secret;

use std::sync::{Arc, Mutex};

use serde_json::Value;
use tauri::{AppHandle, Manager, State, WebviewUrl, WebviewWindow, WebviewWindowBuilder, WindowEvent};
use tauri_plugin_dialog::{DialogExt, FilePath};
use tauri_plugin_opener::OpenerExt;
use tokio::sync::oneshot;

use backend::Backend;

const META_HOSTS: [&str; 4] = ["meta.com", "facebook.com", "oculus.com", "instagram.com"];

fn meta_host(url: &url::Url) -> bool {
    url.scheme() == "https"
        && url.host_str().is_some_and(|host| META_HOSTS.iter().any(|d| host == *d || host.ends_with(&format!(".{d}"))))
}

/// The UI's single entry point. Most methods go straight to the backend; the ones
/// that need a native window, dialog or the system shell are handled here first.
#[tauri::command]
async fn call(app: AppHandle, window: WebviewWindow, backend: State<'_, Backend>, method: String, args: Vec<Value>) -> Result<Value, String> {
    if window.label() != "main" {
        return Err("Untrusted launcher request.".into());
    }
    match method.as_str() {
        "login" => login(&app, &window, &backend).await,
        "import" => match pick(&window, Pick::Apk).await {
            Some(paths) => backend.request("import", paths).await,
            None => Ok(Value::Null),
        },
        "importAssets" => {
            let id = args.first().cloned().unwrap_or(Value::Null);
            match pick(&window, Pick::Assets).await {
                Some(paths) => backend.request("importAssets", vec![id, Value::Array(paths)]).await,
                None => Ok(Value::Null),
            }
        }
        "chooseFolder" => Ok(pick(&window, Pick::Folder).await.and_then(|p| p.into_iter().next()).unwrap_or(Value::Null)),
        "chooseCli" => Ok(pick(&window, Pick::Cli).await.and_then(|p| p.into_iter().next()).unwrap_or(Value::Null)),
        "openFolder" | "openStore" => {
            let target = backend.request(&method, args).await?;
            if let Some(path) = target.get("openPath").and_then(Value::as_str) {
                app.opener().open_path(path, None::<&str>).map_err(|e| e.to_string())?;
            } else if let Some(link) = target.get("openUrl").and_then(Value::as_str) {
                let parsed = url::Url::parse(link).map_err(|e| e.to_string())?;
                if parsed.host_str() != Some("www.meta.com") || parsed.scheme() != "https" {
                    return Err("Refusing to open an unexpected link.".into());
                }
                app.opener().open_url(link, None::<&str>).map_err(|e| e.to_string())?;
            }
            Ok(Value::Null)
        }
        "authBegin" | "authComplete" => Err("Use Connect Meta to sign in.".into()),
        _ => backend.request(&method, args).await,
    }
}

enum Pick { Apk, Assets, Folder, Cli }

async fn pick(window: &WebviewWindow, kind: Pick) -> Option<Vec<Value>> {
    let (sender, receiver) = oneshot::channel::<Option<Vec<FilePath>>>();
    let dialog = window.dialog().file().set_parent(window);
    match kind {
        Pick::Apk => dialog.set_title("Import an Android game").add_filter("Android APK", &["apk"]).pick_file(|f| { let _ = sender.send(f.map(|f| vec![f])); }),
        Pick::Assets => dialog.set_title("Add expansion files / DLC assets").pick_files(|f| { let _ = sender.send(f); }),
        Pick::Folder => dialog.set_title("Choose a folder").pick_folder(|f| { let _ = sender.send(f.map(|f| vec![f])); }),
        Pick::Cli => dialog.set_title("Choose the ovrport CLI").add_filter("ovrport CLI", &["exe", "jar"]).pick_file(|f| { let _ = sender.send(f.map(|f| vec![f])); }),
    }
    let files = receiver.await.ok()??;
    Some(files.into_iter().filter_map(|f| f.into_path().ok()).map(|p| Value::String(p.to_string_lossy().into_owned())).collect())
}

/// Meta sign-in runs on Meta's own page in a separate, private window with no
/// access to the launcher. Its oculus:// callback is caught here and handed to
/// the backend, which exchanges it for the account token.
async fn login(app: &AppHandle, main: &WebviewWindow, backend: &Backend) -> Result<Value, String> {
    if let Some(existing) = app.get_webview_window("meta-auth") {
        let _ = existing.set_focus();
        return Err("Finish signing in to Meta in the open window.".into());
    }
    let start = backend.request("authBegin", vec![]).await?;
    let start = url::Url::parse(start.as_str().ok_or("Meta sign-in could not start.")?).map_err(|e| e.to_string())?;
    let (sender, receiver) = oneshot::channel::<Option<String>>();
    let sender = Arc::new(Mutex::new(Some(sender)));
    let on_callback = sender.clone();
    let window = WebviewWindowBuilder::new(app, "meta-auth", WebviewUrl::External(start))
        .title("Sign in to Meta")
        .inner_size(560.0, 800.0)
        .incognito(true)
        .parent(main)
        .map_err(|e| e.to_string())?
        .on_navigation(move |target| match target.scheme() {
            "oculus" | "oculus-client" => {
                if let Some(sender) = on_callback.lock().unwrap().take() {
                    let _ = sender.send(Some(target.to_string()));
                }
                false
            }
            "about" => true,
            _ => meta_host(target),
        })
        .on_new_window(|_, _| tauri::webview::NewWindowResponse::Deny)
        .build()
        .map_err(|_| "Could not open Meta sign-in.".to_string())?;
    let on_close = sender.clone();
    window.on_window_event(move |event| {
        if matches!(event, WindowEvent::Destroyed) {
            if let Some(sender) = on_close.lock().unwrap().take() {
                let _ = sender.send(None);
            }
        }
    });
    let callback = receiver.await.ok().flatten();
    let _ = window.close();
    let callback = callback.ok_or("Sign-in was cancelled.")?;
    backend.request("authComplete", vec![Value::String(callback)]).await
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, _, _| {
            if let Some(window) = app.get_webview_window("main") {
                let _ = window.unminimize();
                let _ = window.set_focus();
            }
        }))
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .setup(|app| {
            // Same profile folder as earlier launchers: %APPDATA%\Refract.
            let data_dir = app.path().data_dir()?.join("Refract");
            let backend = Backend::start(app.handle().clone(), data_dir)?;
            app.manage(backend);
            Ok(())
        })
        .on_window_event(|window, event| {
            if window.label() == "main" && matches!(event, WindowEvent::Destroyed) {
                window.state::<Backend>().shutdown();
            }
        })
        .invoke_handler(tauri::generate_handler![call])
        .run(tauri::generate_context!())
        .expect("error while running Refract");
}
