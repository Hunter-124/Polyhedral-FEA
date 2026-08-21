// SPDX-License-Identifier: BSD-3-Clause
//
// GLM-vs-Eigen probe (ADR-0044).
//
// GLM is NOT a dependency of this project. This file is a standalone probe that
// `run_probe.sh` compiles against a scratch copy of GLM, so the ADR's numbers
// can be re-measured instead of trusted. Nothing in `src/` or `apps/` includes
// it, and the product build never sees it.
//
// It answers two questions:
//
//   A. On the fixed-size kernels GLM can actually express, is GLM faster than
//      Eigen, and how large a share of the FE inner loop are those kernels?
//   B. GLM's one non-trivial offer -- glm::findEigenvaluesSymReal, a symmetric
//      3x3 eigensolver -- is it accurate enough to stand in for
//      Eigen::SelfAdjointEigenSolver at the call sites we actually have
//      (adapt/src/metric_field.cpp, fea/src/stress.cpp)?
//
// Anti-cheat boundary (CONTRIBUTING.md sec.4): this probe reads nothing from
// bench/reference/, computes no physics accuracy metric, and emits no row in the
// competitive scoreboard schema. It measures library throughput and the
// eigenvalue error of both libraries against an INDEPENDENTLY CONSTRUCTED
// spectrum, so neither library is declared correct by fiat. All random data is
// drawn from explicit fixed seeds.

#include <Eigen/Dense>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/pca.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/// Best-of, so an unlucky scheduler slice cannot make either library look bad.
constexpr int kReps = 7;
constexpr std::size_t kN = 200000;
constexpr std::uint64_t kThroughputSeed = 12345;
constexpr std::uint64_t kSpectrumSeed = 987654321;

// ============================================================ throughput data

/// Trilinear hex8 reference nodes, the element the mesher emits most.
constexpr double kRef[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                               {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};

struct Data {
    /// Distorted hex8 node coordinates: an undistorted element would let the
    /// Jacobian collapse to a diagonal and flatter both libraries equally.
    std::vector<Eigen::Matrix<double, 8, 3>> e_hex;
    std::vector<std::array<glm::dvec3, 8>> g_hex;
    /// dN/dxi at one Gauss point, the table element_stiffness contracts against.
    Eigen::Matrix<double, 8, 3> e_dn;
    std::array<glm::dvec3, 8> g_dn;
    /// Tet vertex quadruples for the orientation predicate.
    std::vector<std::array<Eigen::Vector3d, 4>> e_tet;
    std::vector<std::array<glm::dvec3, 4>> g_tet;
    /// Point stream for bbox / normal work.
    std::vector<Eigen::Vector3d> e_pts;
    std::vector<glm::dvec3> g_pts;
    /// Float vertices for the GUI transform path.
    std::vector<Eigen::Vector3f> e_vtx;
    std::vector<glm::vec3> g_vtx;
    /// Symmetric tensors for the eigendecomposition throughput comparison.
    std::vector<Eigen::Matrix3d> e_sym;
    std::vector<glm::dmat3> g_sym;
};

Data g;

glm::dmat3 to_glm(const Eigen::Matrix3d& m) {
    glm::dmat3 out(0.0);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            // GLM is column-major and indexes [column][row].
            out[static_cast<glm::length_t>(c)][static_cast<glm::length_t>(r)] = m(r, c);
        }
    }
    return out;
}

void build_data(std::size_t n) {
    std::mt19937_64 rng(kThroughputSeed);
    std::uniform_real_distribution<double> jitter(-0.12, 0.12);
    std::uniform_real_distribution<double> box(-1.0, 1.0);

    g.e_hex.resize(n);
    g.g_hex.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        for (int a = 0; a < 8; ++a) {
            for (int c = 0; c < 3; ++c) {
                const double v = kRef[a][c] * 0.5 + jitter(rng);
                g.e_hex[i](a, c) = v;
                g.g_hex[i][static_cast<std::size_t>(a)][c] = v;
            }
        }
    }

    constexpr double gp = 0.57735026918962576451;
    for (int a = 0; a < 8; ++a) {
        const double xa = kRef[a][0], ya = kRef[a][1], za = kRef[a][2];
        const double d[3] = {0.125 * xa * (1 + ya * gp) * (1 + za * gp),
                             0.125 * (1 + xa * gp) * ya * (1 + za * gp),
                             0.125 * (1 + xa * gp) * (1 + ya * gp) * za};
        for (int c = 0; c < 3; ++c) {
            g.e_dn(a, c) = d[c];
            g.g_dn[static_cast<std::size_t>(a)][c] = d[c];
        }
    }

    g.e_tet.resize(n);
    g.g_tet.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        for (int v = 0; v < 4; ++v) {
            for (int c = 0; c < 3; ++c) {
                const double x = jitter(rng) + (v == c + 1 ? 1.0 : 0.0);
                g.e_tet[i][static_cast<std::size_t>(v)][c] = x;
                g.g_tet[i][static_cast<std::size_t>(v)][c] = x;
            }
        }
    }

    g.e_pts.resize(n);
    g.g_pts.resize(n);
    g.e_vtx.resize(n);
    g.g_vtx.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            const double v = box(rng);
            g.e_pts[i][c] = v;
            g.g_pts[i][c] = v;
            g.e_vtx[i][c] = static_cast<float>(v);
            g.g_vtx[i][c] = static_cast<float>(v);
        }
    }

    // M = A^T A + I: symmetric, well conditioned. The hard conditioning classes
    // are handled separately in the spectrum section, which knows its own truth.
    g.e_sym.resize(n);
    g.g_sym.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        Eigen::Matrix3d a;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                a(r, c) = box(rng);
            }
        }
        g.e_sym[i] = (a.transpose() * a) + Eigen::Matrix3d::Identity();
        g.g_sym[i] = to_glm(g.e_sym[i]);
    }
}

// ================================================================== kernels
// Each returns an accumulated value that main() prints, so no loop can be
// dead-code eliminated.

/// K1: the fixed-size core of element_stiffness -- J = dN^T X, det, inverse,
/// dN/dx = dN J^-T. src/fea/src/assembly.cpp:127-137.
double k1_eigen(std::size_t n) {
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const Eigen::Matrix3d jac = g.e_dn.transpose() * g.e_hex[i];
        const double det = jac.determinant();
        const Eigen::Matrix3d jac_inv = jac.inverse();
        const Eigen::Matrix<double, 8, 3> dndx = g.e_dn * jac_inv.transpose();
        acc += det + dndx.sum();
    }
    return acc;
}

double k1_glm(std::size_t n) {
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        // No dN matrix type exists in GLM, so the contraction is a hand loop of
        // rank-1 updates. glm::outerProduct(a, b) yields element (i,j) = a[i]b[j].
        glm::dmat3 jac(0.0);
        for (std::size_t a = 0; a < 8; ++a) {
            jac += glm::outerProduct(g.g_dn[a], g.g_hex[i][a]);
        }
        const double det = glm::determinant(jac);
        const glm::dmat3 jac_inv_t = glm::transpose(glm::inverse(jac));
        double sum = 0.0;
        for (std::size_t a = 0; a < 8; ++a) {
            const glm::dvec3 row = jac_inv_t * g.g_dn[a];
            sum += row.x + row.y + row.z;
        }
        acc += det + sum;
    }
    return acc;
}

/// K2: tet orientation predicate, src/fea/src/assembly.cpp:88.
double k2_eigen(std::size_t n) {
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& t = g.e_tet[i];
        acc += (t[1] - t[0]).dot((t[2] - t[0]).cross(t[3] - t[0]));
    }
    return acc;
}

double k2_glm(std::size_t n) {
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& t = g.g_tet[i];
        acc += glm::dot(t[1] - t[0], glm::cross(t[2] - t[0], t[3] - t[0]));
    }
    return acc;
}

/// K3: bbox accumulation, apps/gui/viewport.hpp:216 Bounds::add.
double k3_eigen(std::size_t n) {
    Eigen::Vector3d lo = g.e_pts[0];
    Eigen::Vector3d hi = lo;
    for (std::size_t i = 1; i < n; ++i) {
        lo = lo.cwiseMin(g.e_pts[i]);
        hi = hi.cwiseMax(g.e_pts[i]);
    }
    return lo.sum() + hi.sum();
}

double k3_glm(std::size_t n) {
    glm::dvec3 lo = g.g_pts[0];
    glm::dvec3 hi = lo;
    for (std::size_t i = 1; i < n; ++i) {
        lo = glm::min(lo, g.g_pts[i]);
        hi = glm::max(hi, g.g_pts[i]);
    }
    return lo.x + lo.y + lo.z + hi.x + hi.y + hi.z;
}

/// K4: triangle face normal by Newell's method, mirroring
/// fea::surface_face_normal (src/fea/src/traction.cpp:628-635): sum of p x q
/// over the corner loop, then normalize. The loop is unrolled in every version
/// so no integer division sits in the measurement.
///
/// This row is where GLM's largest margin comes from, and the margin is NOT
/// GLM being clever -- see k4_scalar below. Eigen 3.5.0's vectorized
/// `Vector3d::cross` is the outlier: a 3-double vector is 24 bytes, so Eigen
/// synthesises the cross from partial SSE loads, shuffles and a scalar tail.
/// Measured on this toolchain (GCC 16.1.1, -O3, no -march), a triple
/// cross-accumulate costs 12.9 ns/op as written, 1.59 ns/op with
/// EIGEN_DONT_VECTORIZE, and 15.1 ns/op with -march=x86-64-v3. GLM has no
/// double-precision SIMD path for mat/vec3 at all, so it emits the scalar code
/// that happens to be the fast one here.
double k4_eigen(std::size_t n) {
    double acc = 0.0;
    const Eigen::Vector3d axis(0.0, 0.0, 1.0);
    for (std::size_t i = 0; i + 2 < n; ++i) {
        const Eigen::Vector3d& p0 = g.e_pts[i];
        const Eigen::Vector3d& p1 = g.e_pts[i + 1];
        const Eigen::Vector3d& p2 = g.e_pts[i + 2];
        Eigen::Vector3d nrm = p0.cross(p1);
        nrm += p1.cross(p2);
        nrm += p2.cross(p0);
        if (nrm.squaredNorm() > 0.0) {
            acc += std::abs(nrm.normalized().dot(axis));
        }
    }
    return acc;
}

double k4_glm(std::size_t n) {
    double acc = 0.0;
    const glm::dvec3 axis(0.0, 0.0, 1.0);
    for (std::size_t i = 0; i + 2 < n; ++i) {
        const glm::dvec3& p0 = g.g_pts[i];
        const glm::dvec3& p1 = g.g_pts[i + 1];
        const glm::dvec3& p2 = g.g_pts[i + 2];
        glm::dvec3 nrm = glm::cross(p0, p1);
        nrm += glm::cross(p1, p2);
        nrm += glm::cross(p2, p0);
        if (glm::dot(nrm, nrm) > 0.0) {
            acc += std::abs(glm::dot(glm::normalize(nrm), axis));
        }
    }
    return acc;
}

/// K4 control: the same normal with the cross products written out by hand, so
/// the table can say whether either library is beating plain scalar code.
double k4_scalar(std::size_t n) {
    double acc = 0.0;
    for (std::size_t i = 0; i + 2 < n; ++i) {
        const double* a = g.e_pts[i].data();
        const double* b = g.e_pts[i + 1].data();
        const double* c = g.e_pts[i + 2].data();
        const double x = a[1] * b[2] - a[2] * b[1] + b[1] * c[2] - b[2] * c[1] +
                         c[1] * a[2] - c[2] * a[1];
        const double y = a[2] * b[0] - a[0] * b[2] + b[2] * c[0] - b[0] * c[2] +
                         c[2] * a[0] - c[0] * a[2];
        const double z = a[0] * b[1] - a[1] * b[0] + b[0] * c[1] - b[1] * c[0] +
                         c[0] * a[1] - c[1] * a[0];
        const double sq = x * x + y * y + z * z;
        if (sq > 0.0) {
            acc += std::abs(z / std::sqrt(sq));
        }
    }
    return acc;
}

/// K5: GUI camera transform, float. apps/gui/viewport.cpp:512-552.
double k5_eigen(std::size_t n) {
    Eigen::Matrix4f view = Eigen::Matrix4f::Identity();
    const Eigen::Vector3f eye(3.f, 2.f, 4.f), tgt(0.f, 0.f, 0.f), up(0.f, 0.f, 1.f);
    const Eigen::Vector3f f = (tgt - eye).normalized();
    const Eigen::Vector3f s = f.cross(up).normalized();
    const Eigen::Vector3f u = s.cross(f);
    view.block<1, 3>(0, 0) = s.transpose();
    view.block<1, 3>(1, 0) = u.transpose();
    view.block<1, 3>(2, 0) = -f.transpose();
    view(0, 3) = -s.dot(eye);
    view(1, 3) = -u.dot(eye);
    view(2, 3) = f.dot(eye);
    Eigen::Matrix4f proj = Eigen::Matrix4f::Zero();
    const float t = 1.f / std::tan(0.5f * 0.9f);
    proj(0, 0) = t / 1.7778f;
    proj(1, 1) = t;
    proj(2, 2) = -(100.f + 0.1f) / (100.f - 0.1f);
    proj(2, 3) = -2.f * 100.f * 0.1f / (100.f - 0.1f);
    proj(3, 2) = -1.f;
    const Eigen::Matrix4f mvp = proj * view;
    float acc = 0.f;
    for (std::size_t i = 0; i < n; ++i) {
        const Eigen::Vector4f clip = mvp * g.e_vtx[i].homogeneous();
        acc += clip.x() / clip.w();
    }
    return static_cast<double>(acc);
}

double k5_glm(std::size_t n) {
    const glm::mat4 view =
        glm::lookAt(glm::vec3(3.f, 2.f, 4.f), glm::vec3(0.f), glm::vec3(0.f, 0.f, 1.f));
    const glm::mat4 mvp = glm::perspective(0.9f, 1.7778f, 0.1f, 100.f) * view;
    float acc = 0.f;
    for (std::size_t i = 0; i < n; ++i) {
        const glm::vec4 clip = mvp * glm::vec4(g.g_vtx[i], 1.f);
        acc += clip.x / clip.w;
    }
    return static_cast<double>(acc);
}

/// K6: symmetric 3x3 eigendecomposition throughput.
double k6_eigen(std::size_t n) {
    double acc = 0.0;
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver;
    for (std::size_t i = 0; i < n; ++i) {
        solver.compute(g.e_sym[i]);
        const Eigen::Vector3d ev = solver.eigenvalues(); // ascending
        acc += ev[0] + 2.0 * ev[1] + 3.0 * ev[2];
    }
    return acc;
}

double k6_glm(std::size_t n) {
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        glm::dvec3 evals(0.0);
        glm::dmat3 evecs(1.0);
        if (glm::findEigenvaluesSymReal(g.g_sym[i], evals, evecs) != 3) {
            std::printf("  !! glm::findEigenvaluesSymReal failed at i=%zu\n", i);
            continue;
        }
        // findEigenvaluesSymReal leaves the output UNORDERED; sortEigenvalues is
        // a separate required call. Eigen guarantees ascending order, so the sort
        // is part of the honest cost of matching Eigen's contract.
        glm::sortEigenvalues(evals, evecs); // descending
        acc += evals[2] + 2.0 * evals[1] + 3.0 * evals[0];
    }
    return acc;
}

/// K7: the full element_stiffness inner loop including B^T D B (6x24), and K8:
/// the same loop with the product removed. K7-K8 is the cost of the product,
/// which is the part GLM has no type for at all.
Eigen::Matrix<double, 6, 6> make_d() {
    constexpr double e = 210e9, nu = 0.3;
    const double lam = e * nu / ((1 + nu) * (1 - 2 * nu));
    const double mu = e / (2 * (1 + nu));
    Eigen::Matrix<double, 6, 6> d = Eigen::Matrix<double, 6, 6>::Zero();
    d.topLeftCorner<3, 3>().setConstant(lam);
    d(0, 0) = d(1, 1) = d(2, 2) = lam + 2 * mu;
    d(3, 3) = d(4, 4) = d(5, 5) = mu;
    return d;
}

/// Voigt strain-displacement matrix, mirroring fea::b_matrix for hex8.
Eigen::Matrix<double, 6, 24> b_matrix(const Eigen::Matrix<double, 8, 3>& dndx) {
    Eigen::Matrix<double, 6, 24> b = Eigen::Matrix<double, 6, 24>::Zero();
    for (int a = 0; a < 8; ++a) {
        const double dx = dndx(a, 0), dy = dndx(a, 1), dz = dndx(a, 2);
        b(0, 3 * a + 0) = dx;
        b(1, 3 * a + 1) = dy;
        b(2, 3 * a + 2) = dz;
        b(3, 3 * a + 1) = dz;
        b(3, 3 * a + 2) = dy;
        b(4, 3 * a + 0) = dz;
        b(4, 3 * a + 2) = dx;
        b(5, 3 * a + 0) = dy;
        b(5, 3 * a + 1) = dx;
    }
    return b;
}

double k7_eigen_full_loop(std::size_t n) {
    const Eigen::Matrix<double, 6, 6> d = make_d();
    Eigen::Matrix<double, 24, 24> k;
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const Eigen::Matrix3d jac = g.e_dn.transpose() * g.e_hex[i];
        const Eigen::Matrix3d jac_inv = jac.inverse();
        const Eigen::Matrix<double, 8, 3> dndx = g.e_dn * jac_inv.transpose();
        const auto b = b_matrix(dndx);
        k.noalias() = b.transpose() * d * b;
        acc += k(0, 0) + k(23, 23);
    }
    return acc;
}

double k8_eigen_no_product(std::size_t n) {
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const Eigen::Matrix3d jac = g.e_dn.transpose() * g.e_hex[i];
        const Eigen::Matrix3d jac_inv = jac.inverse();
        const Eigen::Matrix<double, 8, 3> dndx = g.e_dn * jac_inv.transpose();
        acc += b_matrix(dndx).sum();
    }
    return acc;
}

// ================================================================== timing

double best_of(double (*fn)(std::size_t), std::size_t work, double& sink) {
    double best = 1e300;
    for (int r = 0; r < kReps; ++r) {
        const auto t0 = Clock::now();
        sink += fn(work);
        const auto t1 = Clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() /
                          static_cast<double>(work);
        best = std::min(best, ns);
    }
    return best;
}

struct Row {
    const char* name;
    double eigen_ns;
    double glm_ns;
};

// ============================================ eigensolver spectrum accuracy

/// A symmetric tensor built from a KNOWN spectrum: M = Q diag(lambda) Q^T.
/// Both libraries are then scored against lambda, so neither is the reference.
struct SpectrumCase {
    Eigen::Matrix3d m;
    Eigen::Vector3d truth; // ascending
};

SpectrumCase make_spectrum_case(std::mt19937_64& rng, double l0, double l1, double l2) {
    std::normal_distribution<double> gauss(0.0, 1.0);
    Eigen::Matrix3d a;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            a(r, c) = gauss(rng);
        }
    }
    const Eigen::Matrix3d q = Eigen::HouseholderQR<Eigen::Matrix3d>(a).householderQ();
    Eigen::Vector3d lam(l0, l1, l2);
    std::sort(lam.data(), lam.data() + 3);
    SpectrumCase c;
    c.m = q * lam.asDiagonal() * q.transpose();
    c.m = 0.5 * (c.m + c.m.transpose()); // exact symmetry
    c.truth = lam;
    return c;
}

struct SpectrumScore {
    double worst_rel = 0.0;
    unsigned over_tol = 0;
    unsigned reported_failures = 0;
    unsigned total = 0;
};

struct SpectrumResult {
    const char* label;
    SpectrumScore eigen;
    SpectrumScore glm;
};

/// Anything above this is a wrong answer for metric/stress work: the metric
/// pipeline itself gates on rcond > 1e-12 (adapt/src/metric_field.cpp:290).
constexpr double kTol = 1e-10;

SpectrumResult score_class(const char* label, const std::vector<SpectrumCase>& cases) {
    SpectrumResult out{label, {}, {}};
    for (const auto& c : cases) {
        out.eigen.total += 3;
        out.glm.total += 3;

        const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(c.m);
        if (es.info() != Eigen::Success) {
            ++out.eigen.reported_failures;
        } else {
            const Eigen::Vector3d ev = es.eigenvalues(); // ascending
            for (int k = 0; k < 3; ++k) {
                const double rel = std::abs(ev[k] - c.truth[k]) / std::abs(c.truth[k]);
                out.eigen.worst_rel = std::max(out.eigen.worst_rel, rel);
                out.eigen.over_tol += (rel > kTol);
            }
        }

        glm::dvec3 gev(0.0);
        glm::dmat3 gvec(1.0);
        if (glm::findEigenvaluesSymReal(to_glm(c.m), gev, gvec) != 3) {
            ++out.glm.reported_failures;
        } else {
            glm::sortEigenvalues(gev, gvec); // descending
            for (int k = 0; k < 3; ++k) {
                const double rel = std::abs(gev[2 - k] - c.truth[k]) / std::abs(c.truth[k]);
                out.glm.worst_rel = std::max(out.glm.worst_rel, rel);
                out.glm.over_tol += (rel > kTol);
            }
        }
    }
    return out;
}

} // namespace

int main() {
    build_data(kN);

    std::printf("# GLM vs Eigen probe (ADR-0044)\n");
    std::printf("Eigen %d.%d.%d   GLM %d.%d.%d   n=%zu   best-of-%d\n",
                EIGEN_WORLD_VERSION, EIGEN_MAJOR_VERSION, EIGEN_MINOR_VERSION,
                GLM_VERSION_MAJOR, GLM_VERSION_MINOR, GLM_VERSION_PATCH, kN, kReps);
#if defined(GLM_FORCE_INTRINSICS)
    std::printf("GLM_FORCE_INTRINSICS: on\n");
#else
    std::printf("GLM_FORCE_INTRINSICS: off\n");
#endif

    double sink = 0.0;
    const std::vector<Row> rows{
        {"K1 hex8 Jacobian: J=dN^T X, det, inv, dN/dx", best_of(k1_eigen, kN, sink),
         best_of(k1_glm, kN, sink)},
        {"K2 tet orientation predicate", best_of(k2_eigen, kN, sink),
         best_of(k2_glm, kN, sink)},
        {"K3 bbox min/max stream", best_of(k3_eigen, kN, sink), best_of(k3_glm, kN, sink)},
        {"K4 face normal (Newell, materialized)", best_of(k4_eigen, kN, sink),
         best_of(k4_glm, kN, sink)},
        {"K5 GUI mat4 vertex transform (float)", best_of(k5_eigen, kN, sink),
         best_of(k5_glm, kN, sink)},
        {"K6 symmetric 3x3 eigendecomposition", best_of(k6_eigen, kN, sink),
         best_of(k6_glm, kN, sink)},
    };

    std::printf("\n## Throughput, ns/op (GLM/Eigen < 1 means GLM is faster)\n\n");
    std::printf("| %-43s | %9s | %9s | %9s |\n", "kernel", "Eigen", "GLM", "GLM/Eigen");
    std::printf("|%s|%s|%s|%s|\n", std::string(45, '-').c_str(),
                std::string(11, '-').c_str(), std::string(11, '-').c_str(),
                std::string(11, '-').c_str());
    for (const auto& r : rows) {
        std::printf("| %-43s | %9.3f | %9.3f | %8.2fx |\n", r.name, r.eigen_ns, r.glm_ns,
                    r.glm_ns / r.eigen_ns);
    }

    // The control that keeps K4 honest: if hand-written scalar code beats both
    // libraries, K4 is an Eigen codegen result, not a GLM capability result.
    const double k4c = best_of(k4_scalar, kN, sink);
    std::printf("\nK4 control, hand-written scalar cross: %.3f ns/op "
                "(Eigen %.2fx, GLM %.2fx of control)\n",
                k4c, rows[3].eigen_ns / k4c, rows[3].glm_ns / k4c);

    const double full = best_of(k7_eigen_full_loop, kN, sink);
    const double without = best_of(k8_eigen_no_product, kN, sink);
    std::printf("\n## Share of the FE inner loop GLM has no type for\n\n");
    std::printf("full element_stiffness inner loop     %9.3f ns/op\n", full);
    std::printf("same loop minus the B^T D B product   %9.3f ns/op\n", without);
    std::printf("B^T D B (6x24) alone                  %9.3f ns/op  = %.1f%% of the loop\n",
                full - without, 100.0 * (full - without) / full);

    // ------------------------------------------- fixed-size agreement
    // GLM's plain 3x3/3-vector arithmetic is not in question; record that
    // explicitly so the eigensolver verdict below is not mistaken for a general
    // claim about GLM's accuracy.
    {
        double worst_det = 0.0, worst_inv = 0.0;
        unsigned pred_bitwise = 0;
        const std::size_t m = 4096;
        for (std::size_t i = 0; i < m; ++i) {
            const Eigen::Matrix3d jac = g.e_dn.transpose() * g.e_hex[i];
            glm::dmat3 gjac(0.0);
            for (std::size_t a = 0; a < 8; ++a) {
                gjac += glm::outerProduct(g.g_dn[a], g.g_hex[i][a]);
            }
            worst_det = std::max(worst_det, std::abs(jac.determinant() -
                                                     glm::determinant(gjac)) /
                                                std::abs(jac.determinant()));
            const Eigen::Matrix3d einv = jac.inverse();
            const glm::dmat3 ginv = glm::inverse(gjac);
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    worst_inv = std::max(
                        worst_inv,
                        std::abs(einv(r, c) - ginv[static_cast<glm::length_t>(c)]
                                                  [static_cast<glm::length_t>(r)]));
                }
            }
            const auto& t = g.e_tet[i];
            const auto& gt = g.g_tet[i];
            pred_bitwise +=
                ((t[1] - t[0]).dot((t[2] - t[0]).cross(t[3] - t[0])) ==
                 glm::dot(gt[1] - gt[0], glm::cross(gt[2] - gt[0], gt[3] - gt[0])));
        }
        std::printf("\n## Fixed-size agreement, %zu hex8 elements\n\n", m);
        std::printf("det(J)              max rel diff  %.3e\n", worst_det);
        std::printf("J^-1                max abs diff  %.3e\n", worst_inv);
        std::printf("orientation predicate bitwise identical  %u/%zu\n", pred_bitwise, m);
    }

    // ------------------------------------------------ eigensolver conditioning
    std::mt19937_64 rng(kSpectrumSeed);
    constexpr int kPerClass = 2000;
    struct ClassSpec {
        const char* label;
        double l0, l1, l2;
    };
    // A metric tensor's eigenvalues are h^-2 per direction, so a mesh aspect
    // ratio A appears as an eigenvalue spread of A^2. The last three classes are
    // the ones metric_field.cpp actually manufactures: `dominates` subtracts two
    // nearly equal metrics (tiny magnitude), and `matrix_log` maps h near 1 to
    // log-eigenvalues near zero.
    const ClassSpec classes[] = {
        {"well-conditioned (spread 1e0)", 1.0, 2.0, 3.0},
        {"aspect 1e1 (spread 1e2)", 1.0, 10.0, 100.0},
        {"aspect 1e2 (spread 1e4)", 1.0, 100.0, 1.0e4},
        {"aspect 1e3 (spread 1e6)", 1.0, 1.0e3, 1.0e6},
        {"aspect 1e4 (spread 1e8)", 1.0, 1.0e4, 1.0e8},
        {"aspect 1e6 (spread 1e12)", 1.0, 1.0e6, 1.0e12},
        {"near-degenerate pair (1e-8)", 1.0, 1.0 + 1e-8, 5.0},
        {"near-degenerate pair (1e-12)", 1.0, 1.0 + 1e-12, 5.0},
        {"tiny magnitude (1e-8 scale)", 1e-8, 2e-8, 3e-8},
        {"large magnitude (1e8 scale)", 1e8, 2e8, 3e8},
    };

    std::printf("\n## Symmetric 3x3 eigenvalues vs a known spectrum, %d tensors/class\n",
                kPerClass);
    std::printf("Scored against M = Q diag(lambda) Q^T, so neither library is the "
                "reference.\n");
    std::printf("'bad' counts eigenvalues with relative error > %.0e.\n\n", kTol);
    std::printf("| %-30s | %11s | %9s | %11s | %9s |\n", "conditioning class",
                "Eigen worst", "Eigen bad", "GLM worst", "GLM bad");
    std::printf("|%s|%s|%s|%s|%s|\n", std::string(32, '-').c_str(),
                std::string(13, '-').c_str(), std::string(11, '-').c_str(),
                std::string(13, '-').c_str(), std::string(11, '-').c_str());

    int glm_silently_wrong_classes = 0;
    for (const auto& cl : classes) {
        std::vector<SpectrumCase> cases;
        cases.reserve(static_cast<std::size_t>(kPerClass));
        for (int i = 0; i < kPerClass; ++i) {
            cases.push_back(make_spectrum_case(rng, cl.l0, cl.l1, cl.l2));
        }
        const auto r = score_class(cl.label, cases);
        std::printf("| %-30s | %11.2e | %4u/%-4u | %11.2e | %4u/%-4u |\n", r.label,
                    r.eigen.worst_rel, r.eigen.over_tol, r.eigen.total, r.glm.worst_rel,
                    r.glm.over_tol, r.glm.total);
        // The dangerous case: GLM is far worse than Eigen AND reported success.
        if (r.glm.worst_rel > 1e3 * std::max(r.eigen.worst_rel, 1e-16) &&
            r.glm.reported_failures == 0) {
            ++glm_silently_wrong_classes;
        }
    }
    std::printf("\nclasses where GLM is >1000x worse than Eigen while reporting "
                "success: %d\n",
                glm_silently_wrong_classes);

    std::printf("\n(sink %.6e)\n", sink);
    return 0;
}
