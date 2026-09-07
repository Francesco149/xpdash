//! Security, Ed25519 identity, keypair management, and fingerprint derivation.

use ed25519_dalek::{Signer, SigningKey, Verifier, VerifyingKey, Signature};
use sha2::{Digest, Sha256};
use std::fs;
use std::path::Path;

/// Represents an Ed25519 host identity keypair.
#[derive(Clone)]
pub struct HostIdentity {
    pub signing_key: SigningKey,
    pub verifying_key: VerifyingKey,
}

impl HostIdentity {
    /// Generate a fresh random Ed25519 host identity.
    pub fn generate() -> Self {
        let mut csprng = rand::rngs::OsRng;
        let signing_key = SigningKey::generate(&mut csprng);
        let verifying_key = signing_key.verifying_key();
        Self {
            signing_key,
            verifying_key,
        }
    }

    /// Load the identity from a 32-byte secret seed file, or generate and save if not present.
    pub fn load_or_generate<P: AsRef<Path>>(path: P) -> std::io::Result<Self> {
        let path = path.as_ref();
        if path.exists() {
            let bytes = fs::read(path)?;
            if bytes.len() == 32 {
                let mut seed = [0u8; 32];
                seed.copy_from_slice(&bytes);
                let signing_key = SigningKey::from_bytes(&seed);
                let verifying_key = signing_key.verifying_key();
                return Ok(Self {
                    signing_key,
                    verifying_key,
                });
            }
        }

        let identity = Self::generate();
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::write(path, identity.signing_key.to_bytes())?;
        Ok(identity)
    }

    /// Return the raw 32-byte Ed25519 public key.
    pub fn public_key_bytes(&self) -> [u8; 32] {
        self.verifying_key.to_bytes()
    }

    /// Compute the 32-byte SHA-256 fingerprint of the Ed25519 public key.
    pub fn fingerprint_bytes(&self) -> [u8; 32] {
        compute_fingerprint(&self.public_key_bytes())
    }

    /// Return the human-readable formatted fingerprint string, e.g. "SHA256:7f9a8b..."
    pub fn fingerprint_string(&self) -> String {
        format_fingerprint(&self.public_key_bytes())
    }

    /// Sign an arbitrary message (e.g. auth challenge nonce).
    pub fn sign(&self, message: &[u8]) -> [u8; 64] {
        let sig: Signature = self.signing_key.sign(message);
        sig.to_bytes()
    }

    /// Verify a signature against a given 32-byte public key.
    pub fn verify(public_key_bytes: &[u8; 32], message: &[u8], signature_bytes: &[u8; 64]) -> bool {
        if let Ok(verifying_key) = VerifyingKey::from_bytes(public_key_bytes) {
            let signature = Signature::from_bytes(signature_bytes);
            verifying_key.verify(message, &signature).is_ok()
        } else {
            false
        }
    }
}

/// Compute the SHA-256 fingerprint (32 bytes) of a 32-byte public key.
pub fn compute_fingerprint(pubkey: &[u8; 32]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(pubkey);
    let result = hasher.finalize();
    let mut out = [0u8; 32];
    out.copy_from_slice(&result);
    out
}

/// Format the fingerprint of a public key as "SHA256:<lowercase hex>".
pub fn format_fingerprint(pubkey: &[u8; 32]) -> String {
    let fp = compute_fingerprint(pubkey);
    format!("SHA256:{}", hex_encode(&fp))
}

/// Encode bytes into lowercase hex string.
pub fn hex_encode(bytes: &[u8]) -> String {
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push_str(&format!("{:02x}", b));
    }
    s
}

/// Decode a hex string into bytes.
pub fn hex_decode(hex: &str) -> Result<Vec<u8>, String> {
    let hex = hex.trim();
    if hex.len() % 2 != 0 {
        return Err("Hex string length must be even".to_string());
    }
    let mut bytes = Vec::with_capacity(hex.len() / 2);
    for i in (0..hex.len()).step_by(2) {
        let byte = u8::from_str_radix(&hex[i..i + 2], 16)
            .map_err(|e| format!("Invalid hex at index {}: {}", i, e))?;
        bytes.push(byte);
    }
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_identity_generation_and_signing() {
        let id = HostIdentity::generate();
        let pubkey = id.public_key_bytes();
        let fp_str = id.fingerprint_string();
        assert!(fp_str.starts_with("SHA256:"));
        assert_eq!(fp_str.len(), 7 + 64);

        let msg = b"xpdash-test-challenge-12345";
        let sig = id.sign(msg);
        assert!(HostIdentity::verify(&pubkey, msg, &sig));

        let wrong_msg = b"corrupted-challenge";
        assert!(!HostIdentity::verify(&pubkey, wrong_msg, &sig));
    }

    #[test]
    fn test_hex_roundtrip() {
        let data = [0xde, 0xad, 0xbe, 0xef, 0x01, 0x23, 0x45, 0x67];
        let hex = hex_encode(&data);
        assert_eq!(hex, "deadbeef01234567");
        let decoded = hex_decode(&hex).unwrap();
        assert_eq!(decoded, data);
    }
}
