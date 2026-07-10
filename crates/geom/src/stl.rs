// SPDX-License-Identifier: AGPL-3.0-or-later

//! Binary and ASCII STL loading with vertex welding.

use crate::{GeomError, TriSurface};
use nalgebra::Point3;
use std::collections::HashMap;
use std::io::Read;
use std::path::Path;

/// Loads an STL file (binary or ASCII, auto-detected) and welds duplicate
/// vertices exactly (bitwise equality — STL repeats vertices verbatim, so
/// exact welding is deterministic and tolerance-free).
pub fn load_stl(path: &Path) -> Result<TriSurface, GeomError> {
    let mut bytes = Vec::new();
    std::fs::File::open(path)?.read_to_end(&mut bytes)?;
    let raw = if is_ascii_stl(&bytes) {
        parse_ascii(&bytes)?
    } else {
        parse_binary(&bytes)?
    };
    Ok(weld(&raw))
}

/// Raw triangle soup: 9 coordinates per triangle.
type Soup = Vec<[f64; 9]>;

fn is_ascii_stl(bytes: &[u8]) -> bool {
    // A binary STL can also start with "solid"; require an ASCII "facet"
    // token in the body to disambiguate.
    bytes.starts_with(b"solid")
        && std::str::from_utf8(&bytes[..bytes.len().min(1024)])
            .map(|s| s.contains("facet"))
            .unwrap_or(false)
}

fn parse_binary(bytes: &[u8]) -> Result<Soup, GeomError> {
    if bytes.len() < 84 {
        return Err(GeomError::MalformedStl(
            "shorter than 84-byte header".into(),
        ));
    }
    let n = u32::from_le_bytes(bytes[80..84].try_into().unwrap()) as usize;
    let expected = 84 + n * 50;
    if bytes.len() < expected {
        return Err(GeomError::MalformedStl(format!(
            "header declares {n} triangles ({expected} bytes) but file has {}",
            bytes.len()
        )));
    }
    let mut soup = Vec::with_capacity(n);
    for t in 0..n {
        let base = 84 + t * 50 + 12; // skip the normal, recompute from vertices
        let mut tri = [0.0f64; 9];
        for (k, v) in tri.iter_mut().enumerate() {
            let o = base + k * 4;
            *v = f32::from_le_bytes(bytes[o..o + 4].try_into().unwrap()) as f64;
        }
        soup.push(tri);
    }
    Ok(soup)
}

fn parse_ascii(bytes: &[u8]) -> Result<Soup, GeomError> {
    let text = std::str::from_utf8(bytes)
        .map_err(|e| GeomError::MalformedStl(format!("invalid utf-8: {e}")))?;
    let mut soup = Vec::new();
    let mut coords: Vec<f64> = Vec::with_capacity(9);
    for line in text.lines() {
        let mut it = line.split_whitespace();
        if it.next() == Some("vertex") {
            for _ in 0..3 {
                let tok = it
                    .next()
                    .ok_or_else(|| GeomError::MalformedStl("vertex with <3 coords".into()))?;
                coords.push(
                    tok.parse()
                        .map_err(|e| GeomError::MalformedStl(format!("bad float {tok:?}: {e}")))?,
                );
            }
            if coords.len() == 9 {
                soup.push(coords[..].try_into().unwrap());
                coords.clear();
            }
        }
    }
    if !coords.is_empty() {
        return Err(GeomError::MalformedStl(
            "facet with incomplete vertex triple".into(),
        ));
    }
    Ok(soup)
}

fn weld(soup: &Soup) -> TriSurface {
    let mut map: HashMap<[u64; 3], u32> = HashMap::new();
    let mut surface = TriSurface::default();
    for tri in soup {
        let mut idx = [0u32; 3];
        for (v, slot) in idx.iter_mut().enumerate() {
            let p = [tri[v * 3], tri[v * 3 + 1], tri[v * 3 + 2]];
            let key = p.map(f64::to_bits);
            *slot = *map.entry(key).or_insert_with(|| {
                surface.vertices.push(Point3::from(p));
                (surface.vertices.len() - 1) as u32
            });
        }
        surface.triangles.push(idx);
    }
    surface
}

#[cfg(test)]
mod tests {
    use super::*;

    const TETRA_ASCII: &str = "\
solid tetra
 facet normal 0 0 -1
  outer loop
   vertex 0 0 0
   vertex 0 1 0
   vertex 1 0 0
  endloop
 endfacet
 facet normal 0 -1 0
  outer loop
   vertex 0 0 0
   vertex 1 0 0
   vertex 0 0 1
  endloop
 endfacet
 facet normal -1 0 0
  outer loop
   vertex 0 0 0
   vertex 0 0 1
   vertex 0 1 0
  endloop
 endfacet
 facet normal 1 1 1
  outer loop
   vertex 1 0 0
   vertex 0 1 0
   vertex 0 0 1
  endloop
 endfacet
endsolid tetra
";

    #[test]
    fn ascii_tetra_welds_to_four_vertices() {
        let soup = parse_ascii(TETRA_ASCII.as_bytes()).unwrap();
        assert_eq!(soup.len(), 4);
        let s = weld(&soup);
        assert_eq!(s.vertices.len(), 4);
        assert_eq!(s.triangles.len(), 4);
        s.validate().unwrap();
    }

    #[test]
    fn binary_roundtrip() {
        // Build a binary STL for one triangle and parse it back.
        let mut bytes = vec![0u8; 84];
        bytes[80..84].copy_from_slice(&1u32.to_le_bytes());
        let mut rec = vec![0u8; 12]; // normal
        for v in [[0f32, 0., 0.], [1., 0., 0.], [0., 1., 0.]] {
            for c in v {
                rec.extend_from_slice(&c.to_le_bytes());
            }
        }
        rec.extend_from_slice(&[0, 0]); // attribute byte count
        bytes.extend_from_slice(&rec);
        let soup = parse_binary(&bytes).unwrap();
        assert_eq!(soup.len(), 1);
        assert_eq!(soup[0][3], 1.0);
    }
}
