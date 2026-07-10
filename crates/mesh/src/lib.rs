// SPDX-License-Identifier: AGPL-3.0-or-later

//! Face-based polyhedral mesh (ADR-0004): the mesh is a list of polygonal
//! faces, each owned by one cell and optionally shared with a neighbour cell.
//! Any convex or non-convex polyhedron is representable, and cell adjacency
//! (needed for assembly and error-estimation patches) is a direct scan of
//! interior faces.
//!
//! Units: coordinates in metres.

use nalgebra::Point3;

/// Index newtypes so vertex/face/cell indices can't be mixed up.
macro_rules! index_type {
    ($name:ident) => {
        #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
        pub struct $name(pub u32);

        impl $name {
            #[inline]
            pub fn idx(self) -> usize {
                self.0 as usize
            }
        }
    };
}

index_type!(VertexId);
index_type!(FaceId);
index_type!(CellId);

/// A polygonal face: ordered vertex loop, owner cell, optional neighbour.
///
/// Orientation convention (OpenFOAM-style): the vertex loop is ordered so the
/// face normal points *out of the owner cell* (toward the neighbour, or out of
/// the domain for boundary faces).
#[derive(Debug, Clone)]
pub struct Face {
    pub vertices: Vec<VertexId>,
    pub owner: CellId,
    /// `None` for boundary faces.
    pub neighbour: Option<CellId>,
}

/// Element shape family of a cell — drives which formulation `fea` uses
/// (isoparametric FEM for the standard zoo, VEM for `Polyhedron`; ADR-0003).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CellKind {
    Tet,
    Hex,
    Prism,
    Pyramid,
    Polyhedron,
}

#[derive(Debug, Clone)]
pub struct Cell {
    pub kind: CellKind,
    pub faces: Vec<FaceId>,
}

/// The mesh: flat arrays of vertices, faces, cells.
#[derive(Debug, Clone, Default)]
pub struct PolyMesh {
    pub vertices: Vec<Point3<f64>>,
    pub faces: Vec<Face>,
    pub cells: Vec<Cell>,
}

#[derive(Debug, thiserror::Error)]
pub enum ValidityError {
    #[error("face {0:?} has fewer than 3 vertices")]
    ShortFace(FaceId),
    #[error("face {face:?} references out-of-range vertex {vertex:?}")]
    BadVertexRef { face: FaceId, vertex: VertexId },
    #[error("face {face:?} references out-of-range cell {cell:?}")]
    BadCellRef { face: FaceId, cell: CellId },
    #[error("cell {cell:?} lists face {face:?} which does not reference it back")]
    OwnershipMismatch { cell: CellId, face: FaceId },
    #[error("cell {0:?} has fewer than 4 faces (cannot bound a volume)")]
    OpenCell(CellId),
}

impl PolyMesh {
    /// Structural validity: index ranges, face/cell cross-references, minimum
    /// topology. Geometric checks (watertightness, positive Jacobians,
    /// conforming interfaces) land in Phase P2 and extend this.
    pub fn check_validity(&self) -> Result<(), ValidityError> {
        let nv = self.vertices.len() as u32;
        let nc = self.cells.len() as u32;
        for (i, face) in self.faces.iter().enumerate() {
            let fid = FaceId(i as u32);
            if face.vertices.len() < 3 {
                return Err(ValidityError::ShortFace(fid));
            }
            for &v in &face.vertices {
                if v.0 >= nv {
                    return Err(ValidityError::BadVertexRef {
                        face: fid,
                        vertex: v,
                    });
                }
            }
            for cell in std::iter::once(face.owner).chain(face.neighbour) {
                if cell.0 >= nc {
                    return Err(ValidityError::BadCellRef { face: fid, cell });
                }
            }
        }
        for (i, cell) in self.cells.iter().enumerate() {
            let cid = CellId(i as u32);
            if cell.faces.len() < 4 {
                return Err(ValidityError::OpenCell(cid));
            }
            for &f in &cell.faces {
                let face = self
                    .faces
                    .get(f.idx())
                    .ok_or(ValidityError::OwnershipMismatch { cell: cid, face: f })?;
                if face.owner != cid && face.neighbour != Some(cid) {
                    return Err(ValidityError::OwnershipMismatch { cell: cid, face: f });
                }
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use nalgebra::point;

    /// Single tetrahedron with four boundary faces.
    fn single_tet() -> PolyMesh {
        let c = CellId(0);
        PolyMesh {
            vertices: vec![
                point![0.0, 0.0, 0.0],
                point![1.0, 0.0, 0.0],
                point![0.0, 1.0, 0.0],
                point![0.0, 0.0, 1.0],
            ],
            faces: [[0u32, 2, 1], [0, 1, 3], [0, 3, 2], [1, 2, 3]]
                .into_iter()
                .map(|vs| Face {
                    vertices: vs.into_iter().map(VertexId).collect(),
                    owner: c,
                    neighbour: None,
                })
                .collect(),
            cells: vec![Cell {
                kind: CellKind::Tet,
                faces: (0..4).map(FaceId).collect(),
            }],
        }
    }

    #[test]
    fn single_tet_is_valid() {
        single_tet().check_validity().unwrap();
    }

    #[test]
    fn dangling_vertex_ref_is_caught() {
        let mut m = single_tet();
        m.faces[0].vertices[0] = VertexId(99);
        assert!(matches!(
            m.check_validity(),
            Err(ValidityError::BadVertexRef { .. })
        ));
    }

    #[test]
    fn ownership_mismatch_is_caught() {
        let mut m = single_tet();
        // Add a second cell claiming face 0, which doesn't reference it back.
        m.cells.push(Cell {
            kind: CellKind::Tet,
            faces: vec![FaceId(0), FaceId(1), FaceId(2), FaceId(3)],
        });
        assert!(matches!(
            m.check_validity(),
            Err(ValidityError::OwnershipMismatch { .. })
        ));
    }
}
