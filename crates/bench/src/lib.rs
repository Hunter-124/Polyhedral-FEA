// SPDX-License-Identifier: AGPL-3.0-or-later

//! Verification harness (see BENCHMARKS.md).
//!
//! This is the ONLY crate allowed to read `bench/reference/` — reference
//! values must never appear in, or be read by, `mesh`, `adapt`, or `fea`
//! (engineering rule #1 in CLAUDE.md).

use serde::Deserialize;
use std::path::Path;

/// A reference solution loaded from `bench/reference/*.json`.
#[derive(Debug, Deserialize)]
pub struct ReferenceCase {
    /// Case identifier, e.g. "lame-cylinder".
    pub name: String,
    /// Human-readable derivation or citation for every value below.
    pub citation: String,
    /// Named reference quantities (e.g. "scf" -> 3.0), SI units per field docs.
    pub values: std::collections::BTreeMap<String, f64>,
}

#[derive(Debug, thiserror::Error)]
pub enum BenchError {
    #[error("i/o error loading reference case: {0}")]
    Io(#[from] std::io::Error),
    #[error("malformed reference JSON: {0}")]
    Json(#[from] serde_json::Error),
}

/// Loads one reference case from a JSON file under `bench/reference/`.
pub fn load_reference(path: &Path) -> Result<ReferenceCase, BenchError> {
    let text = std::fs::read_to_string(path)?;
    Ok(serde_json::from_str(&text)?)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_reference_case_json() {
        let json = r#"{
            "name": "kirsch-plate",
            "citation": "Kirsch 1898; SCF = 3 for uniaxial tension, infinite plate, circular hole",
            "values": { "scf": 3.0 }
        }"#;
        let case: ReferenceCase = serde_json::from_str(json).unwrap();
        assert_eq!(case.name, "kirsch-plate");
        assert_eq!(case.values["scf"], 3.0);
    }
}
