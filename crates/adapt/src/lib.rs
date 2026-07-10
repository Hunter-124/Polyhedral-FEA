// SPDX-License-Identifier: AGPL-3.0-or-later

//! Adaptivity: sizing fields, a posteriori error estimation
//! (Zienkiewicz–Zhu patch recovery), Dörfler marking, and per-element
//! h-vs-p refinement decisions.
//!
//! Fills in during Phase P5. Until then this crate carries only the
//! interfaces the mesher (P2/P3) codes against.

use nalgebra::Point3;

/// Target element size as a field over space, metres.
///
/// Produced by geometric feature analysis (a priori, Phase P3) and updated by
/// error estimation (a posteriori, Phase P5).
pub trait SizingField {
    /// Desired local edge length at `point`, metres. Must be strictly positive.
    fn size_at(&self, point: &Point3<f64>) -> f64;
}

/// Constant size everywhere — the trivial field used for uniform baselines.
#[derive(Debug, Clone, Copy)]
pub struct UniformSizing {
    /// Edge length, metres.
    pub h: f64,
}

impl SizingField for UniformSizing {
    fn size_at(&self, _point: &Point3<f64>) -> f64 {
        self.h
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use nalgebra::point;

    #[test]
    fn uniform_sizing_is_constant() {
        let f = UniformSizing { h: 0.05 };
        assert_eq!(f.size_at(&point![0.0, 0.0, 0.0]), 0.05);
        assert_eq!(f.size_at(&point![1e3, -2e3, 5.5]), 0.05);
    }
}
