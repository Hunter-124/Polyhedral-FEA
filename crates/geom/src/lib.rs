// SPDX-License-Identifier: AGPL-3.0-or-later

//! Geometry kernel interface for PolyMesh.
//!
//! Responsibilities:
//! - Load surface geometry (STL now; STEP/B-rep behind the `occ` feature, see ADR-0001).
//! - Discrete feature analysis: sharp edges via dihedral angle, curvature
//!   estimation, thin-wall / proximity detection.
//! - Produce the inputs consumed by `mesh` sizing/classification.
//!
//! Units: SI throughout — coordinates in metres.

pub mod stl;

use nalgebra::Point3;

/// An indexed triangle surface, the common discrete-geometry currency.
///
/// Invariants (enforced by [`TriSurface::validate`]):
/// - all vertex indices in range,
/// - no degenerate (zero-area) triangles.
#[derive(Debug, Clone, Default)]
pub struct TriSurface {
    /// Vertex coordinates in metres.
    pub vertices: Vec<Point3<f64>>,
    /// Counter-clockwise (outward normal) vertex index triples.
    pub triangles: Vec<[u32; 3]>,
}

#[derive(Debug, thiserror::Error)]
pub enum GeomError {
    #[error("triangle {tri} references out-of-range vertex {index}")]
    IndexOutOfRange { tri: usize, index: u32 },
    #[error("triangle {tri} is degenerate (area {area:.3e} m^2 below tolerance)")]
    DegenerateTriangle { tri: usize, area: f64 },
    #[error("i/o error reading geometry: {0}")]
    Io(#[from] std::io::Error),
    #[error("malformed STL: {0}")]
    MalformedStl(String),
}

impl TriSurface {
    /// Checks index validity and rejects degenerate triangles.
    pub fn validate(&self) -> Result<(), GeomError> {
        const AREA_TOL: f64 = 1e-20; // m^2
        let n = self.vertices.len() as u32;
        for (tri, idx) in self.triangles.iter().enumerate() {
            for &i in idx {
                if i >= n {
                    return Err(GeomError::IndexOutOfRange { tri, index: i });
                }
            }
            let [a, b, c] = idx.map(|i| self.vertices[i as usize]);
            let area = 0.5 * (b - a).cross(&(c - a)).norm();
            if area < AREA_TOL {
                return Err(GeomError::DegenerateTriangle { tri, area });
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use nalgebra::point;

    #[test]
    fn validate_rejects_out_of_range_index() {
        let s = TriSurface {
            vertices: vec![point![0.0, 0.0, 0.0], point![1.0, 0.0, 0.0]],
            triangles: vec![[0, 1, 2]],
        };
        assert!(matches!(
            s.validate(),
            Err(GeomError::IndexOutOfRange { tri: 0, index: 2 })
        ));
    }

    #[test]
    fn validate_rejects_degenerate_triangle() {
        let s = TriSurface {
            vertices: vec![
                point![0.0, 0.0, 0.0],
                point![1.0, 0.0, 0.0],
                point![2.0, 0.0, 0.0],
            ],
            triangles: vec![[0, 1, 2]],
        };
        assert!(matches!(
            s.validate(),
            Err(GeomError::DegenerateTriangle { tri: 0, .. })
        ));
    }

    #[test]
    fn validate_accepts_unit_triangle() {
        let s = TriSurface {
            vertices: vec![
                point![0.0, 0.0, 0.0],
                point![1.0, 0.0, 0.0],
                point![0.0, 1.0, 0.0],
            ],
            triangles: vec![[0, 1, 2]],
        };
        s.validate().unwrap();
    }
}
