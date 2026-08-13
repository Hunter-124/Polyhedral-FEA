// SPDX-License-Identifier: BSD-3-Clause

// Regression tests for apps/testlab/run_artifacts.hpp.
//
// Field defect these defend (advisor-batch-1-s3, 2026-08-13): polymesh_testlab
// died twice with exit code 1, no log and no status row. The cause was not a
// crash -- an artifact write threw from inside run_one's exception handler,
// escaped run_one past its own catch-all, and returned 1 from main. The trigger
// was our own monitoring: peer processes reading result.json while the runner
// renamed over it, which on Windows is a MoveFileExW sharing violation.
//
// Two contracts: a transient reader must cost milliseconds rather than a run,
// and an artifact write must never be able to propagate an exception.

#include "run_artifacts.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using polymesh::testlab::atomic_write;
using polymesh::testlab::is_transient_rename_error;
using polymesh::testlab::write_run_json;

namespace {

/// Unique scratch directory; Catch2 runs cases in one process, so sharing a
/// fixed name would let one case observe another's leftovers.
fs::path scratch_dir(const std::string& tag) {
    static std::atomic<int> counter{0};
    const fs::path dir = fs::temp_directory_path() /
                         ("polymesh_run_artifacts_" + tag + "_" +
                          std::to_string(counter.fetch_add(1)));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

std::string read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("run_artifacts: atomic_write replaces an existing file") {
    const fs::path dir = scratch_dir("replace");
    const fs::path target = dir / "result.json";
    atomic_write(target, "first\n");
    CHECK(read_all(target) == "first\n");
    atomic_write(target, "second\n");
    CHECK(read_all(target) == "second\n");
    // The temporary must never be left behind for the recovery tool to trip on.
    CHECK_FALSE(fs::exists(fs::path(target.string() + ".tmp")));
    fs::remove_all(dir);
}

TEST_CASE("run_artifacts: a transient reader costs milliseconds, not the run") {
    // Reproduces the production trigger: the destination is held open by
    // another reader (as a monitoring process would) and released shortly
    // after. The retry must absorb it and the write must still land.
    const fs::path dir = scratch_dir("transient");
    const fs::path target = dir / "result.json";
    atomic_write(target, "old\n");

    std::atomic<bool> opened{false};
    std::atomic<bool> release{false};
    std::thread reader([&] {
        std::ifstream hold(target, std::ios::binary);
        opened.store(true);
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    while (!opened.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Release well inside the ~155 ms retry envelope.
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        release.store(true);
    });

    // On Windows an ifstream does not by itself deny the rename, so this must
    // pass on every platform: the point is that a concurrent reader never makes
    // the write fail. Where it does contend, the retry is what saves it.
    CHECK_NOTHROW(atomic_write(target, "new\n"));
    releaser.join();
    reader.join();
    CHECK(read_all(target) == "new\n");
    fs::remove_all(dir);
}

TEST_CASE("run_artifacts: a permanently blocked destination still reports") {
    // A genuine, non-transient failure must surface rather than be swallowed,
    // so a real I/O fault is never mistaken for success.
    const fs::path dir = scratch_dir("blocked");
    // A directory where the file must go can never be renamed over.
    fs::create_directories(dir / "result.json");
    CHECK_THROWS_AS(atomic_write(dir / "result.json", "{}\n"), std::runtime_error);
    fs::remove_all(dir);
}

TEST_CASE("run_artifacts: write_run_json never throws and reports failure") {
    // THE REGRESSION. write_run_json is called from run_one's exception
    // handlers; if it can throw, the campaign aborts with exit 1 and the status
    // row that the handler exists to produce is lost.
    const fs::path dir = scratch_dir("noexcept");
    const nlohmann::json row{{"cfg_id", "cfg-120dbcb6"},
                             {"part", "sphere_box_s1_c2"},
                             {"tier", 0},
                             {"status", "solve_fail"},
                             {"wall_time_s", 45.8},
                             {"quality", {{"min_scaled_jacobian", 0.21}}}};

    SECTION("happy path writes both artifacts") {
        const fs::path run_dir = dir / "runs" / "cfg-120dbcb6" / "sphere_box_s1_c2" / "t0";
        CHECK(write_run_json(run_dir, row));
        CHECK(fs::exists(run_dir / "result.json"));
        CHECK(fs::exists(run_dir / "quality.json"));
        // Round-trips as the same object the summary row carries.
        CHECK(nlohmann::json::parse(read_all(run_dir / "result.json")) == row);
    }

    SECTION("blocked result.json is reported, not thrown") {
        const fs::path run_dir = dir / "blocked_result";
        fs::create_directories(run_dir / "result.json");
        bool ok = true;
        CHECK_NOTHROW(ok = write_run_json(run_dir, row));
        CHECK_FALSE(ok);
    }

    SECTION("blocked quality.json is reported, not thrown") {
        const fs::path run_dir = dir / "blocked_quality";
        fs::create_directories(run_dir / "quality.json");
        bool ok = true;
        CHECK_NOTHROW(ok = write_run_json(run_dir, row));
        CHECK_FALSE(ok);
        // result.json still landed: a later artifact failing must not undo the
        // one that succeeded, so rebuild_results.py can still recover the row.
        CHECK(fs::exists(run_dir / "result.json"));
    }

    SECTION("an unusable run_dir is reported, not thrown") {
        // A regular file where the run directory must go: create_directories
        // fails, and the old code ignored its error_code and then threw from
        // the write that could not possibly succeed.
        const fs::path blocker = dir / "not_a_dir";
        { std::ofstream out(blocker); out << "x"; }
        bool ok = true;
        CHECK_NOTHROW(ok = write_run_json(blocker / "t0", row));
        CHECK_FALSE(ok);
    }

    SECTION("a row without quality writes only result.json") {
        const fs::path run_dir = dir / "no_quality";
        nlohmann::json lean = row;
        lean.erase("quality");
        CHECK(write_run_json(run_dir, lean));
        CHECK(fs::exists(run_dir / "result.json"));
        CHECK_FALSE(fs::exists(run_dir / "quality.json"));
    }

    fs::remove_all(dir);
}

TEST_CASE("run_artifacts: sharing violations are transient, real faults are not") {
    // permission_denied is what Windows reports for ERROR_SHARING_VIOLATION (32)
    // and ERROR_ACCESS_DENIED (5), the codes a concurrent reader produces.
    CHECK(is_transient_rename_error(std::make_error_code(std::errc::permission_denied)));
    CHECK(is_transient_rename_error(
        std::make_error_code(std::errc::device_or_resource_busy)));
    CHECK(is_transient_rename_error(std::make_error_code(std::errc::no_lock_available)));
    // A missing path or a full disk is not going to fix itself; do not spend the
    // backoff on it.
    CHECK_FALSE(
        is_transient_rename_error(std::make_error_code(std::errc::no_such_file_or_directory)));
    CHECK_FALSE(is_transient_rename_error(std::make_error_code(std::errc::no_space_on_device)));
    CHECK_FALSE(is_transient_rename_error(std::error_code{}));
}
