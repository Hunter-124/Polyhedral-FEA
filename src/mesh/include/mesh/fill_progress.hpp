// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "mesh/hybrid_fill.hpp"

#include <chrono>
#include <format>
#include <string_view>

namespace polymesh::mesh {

using FillProgressTets = std::vector<std::array<std::uint32_t, 4>>;

class FillProgressScope;
/// Innermost live observer for the calling thread, or null (the disabled path).
/// Installed/restored only by `FillProgressScope` (LIFO). A thread that never
/// installed a scope (e.g. an OpenMP worker) sees null, so its polls are no-ops.
/// The callback runs on the installing thread with this pointer cleared (no
/// re-entrant polling).
inline thread_local FillProgressScope* active_fill_progress = nullptr;

/// Observe synchronous fill helpers on this thread. Nested scopes restore their
/// caller on exit; an empty observer suppresses even an enclosing observer.
/// No timer thread: a blocked CAD/kernel call remains honestly silent.
class FillProgressScope {
  public:
    explicit FillProgressScope(const FillOptions& options,
                               const FillProgressTets* tets = nullptr)
        : options_(options), previous_(active_fill_progress), tets_(tets) {
        active_fill_progress = options.on_progress ? this : nullptr;
    }
    ~FillProgressScope() { active_fill_progress = previous_; }
    FillProgressScope(const FillProgressScope&) = delete;
    FillProgressScope& operator=(const FillProgressScope&) = delete;

    void set_cells(std::size_t done, std::size_t total) {
        progress_.cells_done = done;
        progress_.cells_total = total;
    }

    /// Count source for non-tet consumers; an explicit tet binding takes priority.
    void set_elements(std::size_t elements) { progress_.elements_so_far = elements; }

    /// Numbered phases encode actual wave/pass position, not invented elements.
    void set_phase(std::string_view name, int pass = 0, int total = 0) {
        if (!options_.on_progress)
            return;
        progress_.sub_phase =
            total > 0 ? std::format("{} {}/{}", name, pass, total) : std::string(name);
        poll(true);
    }

    void poll(bool force = false) {
        if (!options_.on_progress)
            return;
        const auto now = std::chrono::steady_clock::now();
        // Four seconds leaves room for the next unit of synchronous work.
        if (!force && now - last_ < std::chrono::seconds(4))
            return;
        last_ = now;
        if (tets_ != nullptr)
            progress_.elements_so_far = tets_->size();
        if (removed_ != nullptr)
            progress_.elements_so_far -= *removed_;
        // Consumer work must not recursively poll this observer. Its own nested
        // fill can still install an independent scope and will restore nullptr.
        struct CallbackScope {
            FillProgressScope* saved = active_fill_progress;
            CallbackScope() { active_fill_progress = nullptr; }
            ~CallbackScope() { active_fill_progress = saved; }
        } callback_scope;
        options_.on_progress(progress_);
    }

  private:
    friend class FillProgressElementsScope;
    const FillOptions& options_;
    FillProgressScope* previous_;
    const FillProgressTets* tets_;
    const std::size_t* removed_ = nullptr;
    FillProgress progress_;
    std::chrono::steady_clock::time_point last_{};
};

/// Temporarily bind the live mesh while a helper owns moved-in vectors. A
/// deletion pass may supply its actual tombstone count until compaction ends.
class FillProgressElementsScope {
  public:
    explicit FillProgressElementsScope(const FillProgressTets& tets,
                                       const std::size_t* removed = nullptr)
        : scope_(active_fill_progress), tets_(scope_ != nullptr ? scope_->tets_ : nullptr),
          removed_(scope_ != nullptr ? scope_->removed_ : nullptr) {
        if (scope_ != nullptr) {
            scope_->tets_ = &tets;
            scope_->removed_ = removed;
        }
    }
    ~FillProgressElementsScope() {
        if (scope_ != nullptr) {
            scope_->tets_ = tets_;
            scope_->removed_ = removed_;
        }
    }
    FillProgressElementsScope(const FillProgressElementsScope&) = delete;
    FillProgressElementsScope& operator=(const FillProgressElementsScope&) = delete;

  private:
    FillProgressScope* scope_;
    const FillProgressTets* tets_;
    const std::size_t* removed_;
};

/// Cheap disabled path: no clock read, allocation, or counter scan.
inline void fill_progress_poll() {
    if (active_fill_progress != nullptr)
        active_fill_progress->poll();
}

inline void fill_progress_poll(std::size_t done, std::size_t total) {
    if (active_fill_progress != nullptr) {
        active_fill_progress->set_cells(done, total);
        active_fill_progress->poll();
    }
}

inline void fill_progress_phase(std::string_view name, int pass = 0, int total = 0) {
    if (active_fill_progress != nullptr) {
        active_fill_progress->set_cells(0, 0);
        active_fill_progress->set_phase(name, pass, total);
    }
}

} // namespace polymesh::mesh
