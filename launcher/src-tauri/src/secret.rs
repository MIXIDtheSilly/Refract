// The Meta account token is stored encrypted with Windows DPAPI for the current
// user. It is only passed to the backend process, never to the launcher UI.
use std::path::Path;

const FILE: &str = "meta-session.dpapi";

pub fn load(dir: &Path) -> Option<String> {
    let data = std::fs::read(dir.join(FILE)).ok()?;
    String::from_utf8(unprotect(&data)?).ok()
}

pub fn store(dir: &Path, token: Option<&str>) -> Result<(), String> {
    let file = dir.join(FILE);
    let Some(token) = token else {
        return match std::fs::remove_file(&file) {
            Err(error) if error.kind() != std::io::ErrorKind::NotFound => Err(error.to_string()),
            _ => Ok(()),
        };
    };
    let data = protect(token.as_bytes()).ok_or("Windows credential encryption is unavailable.")?;
    let temporary = dir.join(format!("{FILE}.tmp"));
    std::fs::write(&temporary, data).and_then(|_| std::fs::rename(&temporary, &file)).map_err(|e| e.to_string())
}

#[cfg(windows)]
fn crypt(input: &[u8], encrypt: bool) -> Option<Vec<u8>> {
    use windows_sys::Win32::Foundation::LocalFree;
    use windows_sys::Win32::Security::Cryptography::{
        CryptProtectData, CryptUnprotectData, CRYPTPROTECT_UI_FORBIDDEN, CRYPT_INTEGER_BLOB,
    };
    let blob_in = CRYPT_INTEGER_BLOB { cbData: u32::try_from(input.len()).ok()?, pbData: input.as_ptr() as *mut u8 };
    let mut blob_out = CRYPT_INTEGER_BLOB { cbData: 0, pbData: std::ptr::null_mut() };
    // SAFETY: the input blob outlives the call; the output buffer is copied and then freed with LocalFree.
    unsafe {
        let ok = if encrypt {
            CryptProtectData(&blob_in, std::ptr::null(), std::ptr::null(), std::ptr::null(), std::ptr::null(), CRYPTPROTECT_UI_FORBIDDEN, &mut blob_out)
        } else {
            CryptUnprotectData(&blob_in, std::ptr::null_mut(), std::ptr::null(), std::ptr::null(), std::ptr::null(), CRYPTPROTECT_UI_FORBIDDEN, &mut blob_out)
        };
        if ok == 0 || blob_out.pbData.is_null() {
            return None;
        }
        let output = std::slice::from_raw_parts(blob_out.pbData, blob_out.cbData as usize).to_vec();
        LocalFree(blob_out.pbData as _);
        Some(output)
    }
}

#[cfg(windows)]
fn protect(data: &[u8]) -> Option<Vec<u8>> { crypt(data, true) }
#[cfg(windows)]
fn unprotect(data: &[u8]) -> Option<Vec<u8>> { crypt(data, false) }

// Other platforms are only used for UI development: keep the session in memory.
#[cfg(not(windows))]
fn protect(_: &[u8]) -> Option<Vec<u8>> { None }
#[cfg(not(windows))]
fn unprotect(_: &[u8]) -> Option<Vec<u8>> { None }
