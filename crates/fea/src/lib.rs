// SPDX-License-Identifier: AGPL-3.0-or-later

//! Linear elastostatics solver.
//!
//! One [`Element`] trait covers the whole zoo (ADR-0003): standard
//! isoparametric FEM for tets/hexes/prisms/pyramids at order p = 1..=4, and
//! VEM for general polyhedra. Assembly and solve layers are formulation-blind.
//!
//! All arithmetic is `f64`. Units are SI: metres, pascals, newtons.

use nalgebra::DMatrix;

/// Isotropic linear-elastic material.
#[derive(Debug, Clone, Copy)]
pub struct Material {
    /// Young's modulus, Pa.
    pub youngs_modulus: f64,
    /// Poisson's ratio, dimensionless, in (-1, 0.5).
    pub poissons_ratio: f64,
}

impl Material {
    /// Lamé's first parameter λ, Pa.
    pub fn lambda(&self) -> f64 {
        let (e, nu) = (self.youngs_modulus, self.poissons_ratio);
        e * nu / ((1.0 + nu) * (1.0 - 2.0 * nu))
    }

    /// Shear modulus μ (Lamé's second parameter), Pa.
    pub fn mu(&self) -> f64 {
        self.youngs_modulus / (2.0 * (1.0 + self.poissons_ratio))
    }

    /// 6×6 constitutive matrix D in Voigt notation
    /// (order: xx, yy, zz, yz, xz, xy; engineering shear strains), Pa.
    pub fn d_matrix(&self) -> DMatrix<f64> {
        let (l, m) = (self.lambda(), self.mu());
        let mut d = DMatrix::zeros(6, 6);
        for i in 0..3 {
            for j in 0..3 {
                d[(i, j)] = l;
            }
            d[(i, i)] = l + 2.0 * m;
            d[(i + 3, i + 3)] = m;
        }
        d
    }
}

/// A single element's contribution to the global system.
///
/// Every implementation — isoparametric or VEM, any order — must satisfy the
/// Tier-0 gates in `bench`: exact constant-strain patch test on distorted
/// meshes, zero strain energy for rigid-body modes, and no spurious
/// zero-energy modes (see BENCHMARKS.md).
pub trait Element {
    /// Polynomial order p this instance is configured for.
    fn order(&self) -> u8;

    /// Number of nodes (3 DOFs each).
    fn num_nodes(&self) -> usize;

    /// Element stiffness matrix, size `3*num_nodes()` square, in N/m.
    fn stiffness(&self, material: &Material) -> DMatrix<f64>;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn d_matrix_matches_lame_constants() {
        // Steel-ish: E = 200 GPa, nu = 0.3.
        let m = Material {
            youngs_modulus: 200e9,
            poissons_ratio: 0.3,
        };
        let d = m.d_matrix();
        let (l, mu) = (m.lambda(), m.mu());
        assert!((d[(0, 0)] - (l + 2.0 * mu)).abs() < 1.0);
        assert!((d[(0, 1)] - l).abs() < 1.0);
        assert!((d[(3, 3)] - mu).abs() < 1.0);
        // Symmetry.
        for i in 0..6 {
            for j in 0..6 {
                assert_eq!(d[(i, j)], d[(j, i)]);
            }
        }
    }

    #[test]
    fn uniaxial_stress_recovers_youngs_modulus() {
        // For uniaxial stress σ_xx with free lateral contraction,
        // σ_xx / ε_xx must equal E. Invert D and check compliance.
        let m = Material {
            youngs_modulus: 70e9,
            poissons_ratio: 0.33,
        };
        let d = m.d_matrix();
        let c = d.try_inverse().expect("D must be invertible");
        let e_recovered = 1.0 / c[(0, 0)];
        assert!(
            (e_recovered - m.youngs_modulus).abs() / m.youngs_modulus < 1e-12,
            "recovered E = {e_recovered}"
        );
    }
}
