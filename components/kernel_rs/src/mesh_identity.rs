// SPDX-License-Identifier: BSD-3-Clause
//! The device's identity key: the MeshCore node's Ed25519 key, which
//! Tailnet chat proves too (ADR-0015), so the device is one key on LoRa and
//! on the tailnet.
//!
//! It is `/spiffs/config/mesh_identity`, in the MeshCore engine's own layout
//! (components/meshcore/src/meshcore_shim.cpp, `load_identity`): the magic
//! "MCI1", MeshCore's 64-byte private key, which on ThistleOS is the Ed25519
//! seed followed by the public key (components/meshcore/include/ed_25519.h),
//! then the public key again. 100 bytes.
//!
//! The engine made it the first time the mesh started, so a board that
//! never had the mesh on, or has no radio, had none. Now the kernel makes it
//! at boot, before the mesh starts and whatever the mesh setting, and the
//! engine reads the one made here. Like MeshCore's, a key is never
//! 00... or FF..., the hashes MeshCore keeps for itself.

use std::path::{Path, PathBuf};
use std::sync::Mutex;

use ed25519_dalek::{Signer, SigningKey};

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}
#[cfg(not(test))]
static TAG: &[u8] = b"identity\0";
#[cfg(not(test))]
macro_rules! log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => {
        unsafe { esp_log_write($level, TAG.as_ptr(), $fmt.as_ptr() $(, $arg)*) }
    };
}
#[cfg(test)]
macro_rules! log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => {{
        let _ = ($level, $fmt $(, $arg)*);
    }};
}
const ESP_LOG_ERROR: i32 = 1;
const ESP_LOG_INFO: i32 = 3;

pub(crate) const FILE_NAME: &str = "mesh_identity";
const MAGIC: [u8; 4] = *b"MCI1";
const FILE_LEN: usize = 4 + 64 + 32;

/// The device's key pair. The seed never leaves this module.
#[derive(Clone)]
pub(crate) struct Identity {
    seed: [u8; 32],
    pub public: [u8; 32],
}

impl Identity {
    /// An Ed25519 signature over `msg`.
    pub(crate) fn sign(&self, msg: &[u8]) -> [u8; 64] {
        SigningKey::from_bytes(&self.seed).sign(msg).to_bytes()
    }

    fn from_seed(seed: [u8; 32]) -> Self {
        let public = SigningKey::from_bytes(&seed).verifying_key().to_bytes();
        Self { seed, public }
    }

    /// The file's bytes, in MeshCore's layout.
    fn to_file(&self) -> [u8; FILE_LEN] {
        let mut file = [0u8; FILE_LEN];
        file[..4].copy_from_slice(&MAGIC);
        file[4..36].copy_from_slice(&self.seed);
        file[36..68].copy_from_slice(&self.public);
        file[68..].copy_from_slice(&self.public);
        file
    }

    /// An identity from the file's bytes, if they hold one whose public key
    /// is its seed's.
    fn from_file(file: &[u8]) -> Option<Self> {
        if file.len() != FILE_LEN || file[..4] != MAGIC {
            return None;
        }
        let id = Self::from_seed(file[4..36].try_into().ok()?);
        (file[36..68] == id.public && file[68..] == id.public).then_some(id)
    }
}

/// What `load_or_make_in` did.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Found {
    /// The file held an identity.
    Kept,
    /// There was none, or the file was not one; a new one was saved.
    Made,
}

/// The identity in `dir`, made and saved if there is none.
pub(crate) fn load_or_make_in(dir: &Path) -> Result<(Identity, Found), String> {
    let path = dir.join(FILE_NAME);
    if let Some(id) = std::fs::read(&path).ok().as_deref().and_then(Identity::from_file) {
        return Ok((id, Found::Kept));
    }
    // MeshCore keeps the 00 and FF hashes, a key's first byte, for itself.
    let id = loop {
        let mut seed = [0u8; 32];
        getrandom::fill(&mut seed).map_err(|e| format!("no random numbers: {e}"))?;
        let id = Identity::from_seed(seed);
        if id.public[0] != 0x00 && id.public[0] != 0xFF {
            break id;
        }
    };
    #[cfg(not(target_os = "espidf"))]
    std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    std::fs::write(&path, id.to_file()).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok((id, Found::Made))
}

/// Make sure the device has its identity, at boot, before the mesh starts.
pub(crate) fn ensure() {
    let dir = config_dir();
    match load_or_make_in(&dir) {
        Ok((id, found)) => {
            let p = &id.public;
            let line = format!(
                "{} {:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}...\0",
                if found == Found::Made { "made a new identity key," } else { "identity key" },
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]
            );
            log!(ESP_LOG_INFO, b"%s\0", line.as_ptr());
            if let Ok(mut slot) = IDENTITY.lock() {
                *slot = Some(id);
            }
        }
        Err(why) => {
            let line = format!("no identity key: {why}\0");
            log!(ESP_LOG_ERROR, b"%s\0", line.as_ptr());
        }
    }
}

/// The device's identity: its public key, and signatures with it. None if
/// it could be neither read nor made.
pub(crate) fn identity() -> Option<Identity> {
    IDENTITY.lock().ok().and_then(|slot| slot.clone())
}

/// The device's public identity key, for C (32 bytes into `out`).
///
/// # Safety
/// `out` must be null or point to 32 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn rs_identity_public_key(out: *mut u8) -> i32 {
    const ESP_ERR_INVALID_ARG: i32 = 0x102;
    const ESP_ERR_INVALID_STATE: i32 = 0x103;
    if out.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    match identity() {
        Some(id) => {
            unsafe { std::ptr::copy_nonoverlapping(id.public.as_ptr(), out, 32) };
            0
        }
        None => ESP_ERR_INVALID_STATE,
    }
}

fn config_dir() -> PathBuf {
    if cfg!(target_os = "espidf") {
        PathBuf::from("/spiffs/config")
    } else {
        std::env::var("THISTLE_SIM_SPIFFS")
            .ok()
            .filter(|s| !s.is_empty())
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("/tmp/thistle_spiffs"))
            .join("config")
    }
}

static IDENTITY: Mutex<Option<Identity>> = Mutex::new(None);

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::{Signature, Verifier, VerifyingKey};

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
    }

    fn scratch_dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("thistle_mesh_identity_{}_{}", name, std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    // RFC 8032, 7.1, TEST 1
    const RFC_SEED: &str = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
    const RFC_PUBLIC: &str = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
    const RFC_SIGNATURE: &str = "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555\
                                 fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";

    #[test]
    fn a_board_without_an_identity_is_given_one_in_meshcores_layout() {
        let dir = scratch_dir("new");
        let (id, found) = load_or_make_in(&dir).expect("an identity");
        assert_eq!(found, Found::Made);
        let file = std::fs::read(dir.join(FILE_NAME)).unwrap();
        assert_eq!(file.len(), 100);
        assert_eq!(&file[..4], b"MCI1");
        assert_eq!(&file[36..68], &id.public, "the private key ends with the public key");
        assert_eq!(&file[68..100], &id.public);
        assert!(id.public[0] != 0x00 && id.public[0] != 0xFF);
        // The key signs, and its public half checks the signature.
        let sig = id.sign(b"hello tailnet");
        let key = VerifyingKey::from_bytes(&id.public).unwrap();
        assert!(key.verify(b"hello tailnet", &Signature::from_bytes(&sig)).is_ok());
    }

    #[test]
    fn an_identity_is_kept_across_boots() {
        let dir = scratch_dir("kept");
        let (first, _) = load_or_make_in(&dir).unwrap();
        let file = std::fs::read(dir.join(FILE_NAME)).unwrap();
        let (again, found) = load_or_make_in(&dir).unwrap();
        assert_eq!(found, Found::Kept);
        assert_eq!(again.public, first.public);
        assert_eq!(std::fs::read(dir.join(FILE_NAME)).unwrap(), file);
    }

    #[test]
    fn an_identity_the_mesh_made_is_the_one_used() {
        let dir = scratch_dir("meshcore");
        let mut file = b"MCI1".to_vec();
        file.extend(hex(RFC_SEED));
        file.extend(hex(RFC_PUBLIC));
        file.extend(hex(RFC_PUBLIC));
        std::fs::write(dir.join(FILE_NAME), &file).unwrap();
        let (id, found) = load_or_make_in(&dir).unwrap();
        assert_eq!(found, Found::Kept);
        assert_eq!(id.public.to_vec(), hex(RFC_PUBLIC));
        assert_eq!(id.sign(b"").to_vec(), hex(RFC_SIGNATURE));
    }

    #[test]
    fn a_file_that_is_not_an_identity_is_replaced() {
        let dir = scratch_dir("damaged");
        std::fs::write(dir.join(FILE_NAME), b"MCI1 but far too short").unwrap();
        let (id, found) = load_or_make_in(&dir).unwrap();
        assert_eq!(found, Found::Made);
        assert_eq!(&std::fs::read(dir.join(FILE_NAME)).unwrap()[68..], &id.public);
    }
}
