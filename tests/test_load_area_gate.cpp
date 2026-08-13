// SPDX-License-Identifier: BSD-3-Clause

// The load-area health gate used to report load_area_rel_err = 0.0 whenever it
// could not establish an expected area, with load_area_ok true by default. A zero
// relative error reads as a perfect match, so meshes missing 14-66% of their
// loaded face recorded a flawless area check and passed the gate. Because a case
// traction is a pressure, those meshes applied a proportionally smaller force and
// solved the wrong problem while labelled healthy. The two families that omit
// select.expected_area are exactly the two with curved loaded surfaces, so the
// blindness landed where it did the most damage.
//
// The replacement uses cad_rule_area -- the case's own selection rule evaluated on
// the exact CAD tessellation -- to rescale the traction so the applied resultant
// is correct on any mesh, and reports the residual deficit without failing on it.
//
// The first two cases are the regression proper: an unverifiable area must never
// surface as 0.0, and must never be mistaken for a verified pass.

#include "load_area.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <limits>
#include <optional>

namespace {
using polymesh::testlab::assess_load_area;
using polymesh::testlab::kLoadAreaTol;
using polymesh::testlab::load_area_status_name;
using polymesh::testlab::LoadAreaStatus;
} // namespace

TEST_CASE("load area: an unverifiable area is never reported as zero error") {
    // THE REGRESSION. No authored expected_area and no CAD rule area: nothing can
    // be established, so there must be no number at all.
    const auto none = assess_load_area(std::nullopt, std::nullopt, 1.0e-4);
    CHECK(none.status == LoadAreaStatus::kUnverified);
    CHECK_FALSE(none.rel_err.has_value());          // NOT 0.0
    CHECK(load_area_status_name(none.status) == "unverified");

    // Degenerate areas are equally unverifiable, not perfect matches.
    for (const double bad : {0.0, -1.0}) {
        const auto authored = assess_load_area(bad, std::nullopt, 1.0e-4);
        CHECK(authored.status == LoadAreaStatus::kUnverified);
        CHECK_FALSE(authored.rel_err.has_value());
        const auto rule = assess_load_area(std::nullopt, bad, 1.0e-4);
        CHECK(rule.status == LoadAreaStatus::kUnverified);
        CHECK_FALSE(rule.rel_err.has_value());
    }

    // A missing or non-finite measured area cannot be compared either.
    for (const double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
        const auto out = assess_load_area(1.0e-4, 1.0e-4, bad);
        CHECK(out.status == LoadAreaStatus::kUnverified);
        CHECK_FALSE(out.rel_err.has_value());
    }
}

TEST_CASE("load area: an unverified area does not count as a verified pass") {
    // `ok` stays true so an unknown does not sink an otherwise healthy row, which
    // makes it essential that status remains independently observable: anything
    // reading `ok` alone and calling the area verified is reading a value that was
    // never measured.
    const auto none = assess_load_area(std::nullopt, std::nullopt, 5.0e-5);
    CHECK(none.status != LoadAreaStatus::kVerified);
    CHECK(none.rel_err == std::nullopt);
    CHECK(none.ok);
}

TEST_CASE("load area: a rescaled run reports its deficit and stays healthy") {
    // Once the traction is rescaled onto cad_rule_area the applied RESULTANT is
    // correct by construction, so a large deficit means the traction DISTRIBUTION
    // is coarse, not that the run applied the wrong force. The campaign probes are
    // far-field, so such a row is a legitimate measurement and must stay usable --
    // flagged, never discarded.
    const double rule = 7.5166e-5;
    const auto deficient = assess_load_area(std::nullopt, rule, 4.1394e-5);
    CHECK(deficient.status == LoadAreaStatus::kRescaledToExactCad);
    REQUIRE(deficient.rel_err.has_value());
    CHECK_THAT(*deficient.rel_err, Catch::Matchers::WithinAbs(0.4493, 1e-3));
    CHECK(*deficient.rel_err > kLoadAreaTol);   // a real 45% gap, reported
    CHECK(deficient.ok);                        // and NOT a health failure
    CHECK(load_area_status_name(deficient.status) == "rescaled_to_exact_cad");

    // The worst measured case (66% on a spherical boss) is still not a failure.
    const auto worst = assess_load_area(std::nullopt, 2.104430e-2, 7.099739e-3);
    CHECK(worst.status == LoadAreaStatus::kRescaledToExactCad);
    REQUIRE(worst.rel_err.has_value());
    CHECK_THAT(*worst.rel_err, Catch::Matchers::WithinAbs(0.6626, 1e-3));
    CHECK(worst.ok);

    // A mesh that resolves the rule area exactly reports ~0 and is still rescaled.
    const auto exact = assess_load_area(std::nullopt, rule, rule);
    CHECK(exact.status == LoadAreaStatus::kRescaledToExactCad);
    REQUIRE(exact.rel_err.has_value());
    CHECK(*exact.rel_err < 1e-12);
    CHECK(exact.ok);

    // Over-selection is reported the same way, not treated as a pass.
    const auto over = assess_load_area(std::nullopt, rule, 1.05 * rule);
    CHECK(over.status == LoadAreaStatus::kRescaledToExactCad);
    REQUIRE(over.rel_err.has_value());
    CHECK_THAT(*over.rel_err, Catch::Matchers::WithinAbs(0.05, 1e-12));
}

TEST_CASE("load area: cad_rule_area outranks an authored expected_area") {
    // The rescale target is what the deficit must be measured against, otherwise
    // the reported number describes a comparison the load did not use.
    const auto out = assess_load_area(1.0e-4, 7.5166e-5, 7.5166e-5);
    CHECK(out.status == LoadAreaStatus::kRescaledToExactCad);
    REQUIRE(out.rel_err.has_value());
    CHECK(*out.rel_err < 1e-12);
    CHECK(out.ok);
}

TEST_CASE("load area: an authored area with nothing to rescale onto still gates") {
    // No CAD, so no rescale happened and a deviation IS a wrong resultant. This is
    // the one remaining condition that may fail health, and it must still bite --
    // it is the guard the fixture cases (plate_hole, cylinder) rely on.
    const double expected = 1.0e-4;
    const auto good = assess_load_area(expected, std::nullopt, 1.02e-4);
    CHECK(good.status == LoadAreaStatus::kVerified);
    REQUIRE(good.rel_err.has_value());
    CHECK_THAT(*good.rel_err, Catch::Matchers::WithinAbs(0.02, 1e-12));
    CHECK(good.ok);

    const auto bad = assess_load_area(expected, std::nullopt, 0.72e-4);
    CHECK(bad.status == LoadAreaStatus::kVerified);
    REQUIRE(bad.rel_err.has_value());
    CHECK_THAT(*bad.rel_err, Catch::Matchers::WithinAbs(0.28, 1e-12));
    CHECK_FALSE(bad.ok);                        // the gate still fails here
    CHECK(*bad.rel_err > kLoadAreaTol);

    // Boundary: at the tolerance it passes, just beyond it fails.
    CHECK(assess_load_area(expected, std::nullopt, expected * 1.05).ok);
    CHECK_FALSE(assess_load_area(expected, std::nullopt, expected * 1.0501).ok);
}

TEST_CASE("load area: authored expected_area drift from the CAD cannot pass silently") {
    using polymesh::testlab::check_authored_area;
    using polymesh::testlab::kAuthoredAreaTol;

    // Rescaling onto cad_rule_area stops consulting the authored expected_area, so
    // without this cross-check the authored guard would be quietly retired on the
    // 48 corpus cases that carry one. A disagreement is a case-definition or
    // geometry bug, so it must be reported and must NOT be mistaken for agreement.
    const double cad = 7.556951024525475e-05;

    // Today's corpus: authored and CAD agree to ~5e-10 across all 48 cases.
    const auto agree = check_authored_area(7.556951028e-05, cad);
    CHECK(agree.checked);
    REQUIRE(agree.rel_diff.has_value());
    CHECK(*agree.rel_diff < 1e-8);
    CHECK(agree.consistent);

    // Drift beyond tolerance must be flagged, not absorbed.
    const auto drifted = check_authored_area(cad * 1.25, cad);
    CHECK(drifted.checked);
    REQUIRE(drifted.rel_diff.has_value());
    CHECK_THAT(*drifted.rel_diff, Catch::Matchers::WithinAbs(0.25, 1e-12));
    CHECK_FALSE(drifted.consistent);

    // Boundary either side of the tolerance.
    CHECK(check_authored_area(cad * (1.0 + kAuthoredAreaTol), cad).consistent);
    CHECK_FALSE(check_authored_area(cad * (1.0 + 2.0 * kAuthoredAreaTol), cad).consistent);

    // Not run is NOT agreement: `checked` false and rel_diff EMPTY, so a missing
    // authored value can never be counted as a passing cross-check.
    for (const auto& out : {check_authored_area(std::nullopt, cad),
                            check_authored_area(cad, std::nullopt),
                            check_authored_area(std::nullopt, std::nullopt),
                            check_authored_area(0.0, cad),
                            check_authored_area(cad, 0.0)}) {
        CHECK_FALSE(out.checked);
        CHECK_FALSE(out.rel_diff.has_value());
    }
}
