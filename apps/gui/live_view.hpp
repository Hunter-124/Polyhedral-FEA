// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Live, real-time visualisation of a running mesh + solve job.
//
// This is the interactive sibling of `cinema.hpp`. Cinema is a scripted,
// virtual-clock take recorded offline for the showcase film; LiveView is the
// same visual language driven by the real wall clock while a user's own study
// runs. It draws three things, in the order the job produces them:
//
//   1. the advisor deliberating — the real ONNX activations of every candidate
//      it scored, played back over the network layout;
//   2. the mesh arriving — each `pipeline::MeshStage` the mesher really emitted,
//      revealed cell by cell through the viewport's cinema reveal shader;
//   3. the solve converging — CG residual / adaptive-pass telemetry.
//
// The honesty invariant of cinema applies verbatim here: every number and every
// cell on screen is a field of a `pipeline::MeshStage`, `pipeline::SolveStage`,
// `pipeline::JobProgress` or `advisor::ActivationFrame` that this run really
// produced. Only time, opacity and the reveal front are interpolated.

#include <optional>

#include "advisor/advisor.hpp"
#include "imgui.h"
#include "pipeline/scene.hpp"
#include "viewport.hpp"

namespace polymesh::gui {

class LiveView {
  public:
    LiveView();
    ~LiveView();
    LiveView(const LiveView&) = delete;
    LiveView& operator=(const LiveView&) = delete;

    /// Installs the worker-thread callbacks on `job` and clears per-run state.
    /// Call immediately before `job.start()` / `job.start_mesh()`; never while a
    /// worker is running (the callbacks are read by the worker thread).
    void attach(pipeline::SolveJob& job);

    /// Clears the callbacks. Call once the job has been joined via
    /// `take_result()` / `take_mesh()`, and never while a worker is running.
    void detach(pipeline::SolveJob& job);

    /// Hands over the setup this run was started with, so the overlays can
    /// label what the job is actually aiming at: the ZZ error target and pass
    /// budget the convergence lane marks, the mesher name and the requested
    /// element size. Read for labelling only — `LiveView` never acts on it.
    /// Call before `attach()`; the setup is copied.
    void set_setup(const pipeline::SimSetup& setup);

    /// Hands over the advisor's deliberation for this run, so the activation
    /// card can play back the real forward passes while the mesher works.
    /// `layout` is `Advisor::layout()` — the static network shape and weight
    /// blocks the card ranks its connections from; it is copied because the
    /// `Advisor` that owns it need not outlive the animation.
    /// Either argument may be `std::nullopt` (advisor disabled, or an advisor
    /// without activation taps) — the card then simply does not appear.
    void set_explanation(std::optional<advisor::AdvisorExplanation> explanation,
                         std::optional<advisor::NetworkLayout> layout);

    /// UI thread, once per frame while a job runs. Drains the worker queues,
    /// advances the animation clocks by `dt` seconds, uploads any newly arrived
    /// mesh stage, and writes the reveal uniforms into `vp`.
    /// Returns true when the viewport should render in `DisplayMode::kCinema`
    /// this frame (i.e. the live reveal owns the canvas).
    bool tick(float dt, Viewport& vp);

    /// UI thread. Draws the floating live overlays inside the viewport rect
    /// [`mn`, `mx`] on `dl`: the stage/progress HUD, the advisor activation
    /// card and the convergence lane. Draws nothing when idle. `mono` may be
    /// null; when set it is used for numerics.
    void draw_overlays(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImFont* mono);

    /// True from `attach()` until the last queued frame has played out. The
    /// shell keeps calling `tick()` and `draw_overlays()` while this is true,
    /// which lets the final cells and the closing readout land after the worker
    /// itself has finished.
    bool active() const;

    /// The most recent run supplied a real advisor explanation and network
    /// layout. This stays true after the animation settles so the desktop can
    /// use otherwise-empty rail space for a persistent, auditable snapshot.
    bool has_advisor_content() const;

    /// Draws the advisor instrument inside a dedicated rail rectangle instead
    /// of floating over the model. While a run is active it advances with the
    /// real frames; after completion it settles on the final re-score. The
    /// caller owns the child/clip rect and passes its screen-space bounds.
    void draw_advisor_dock(ImDrawList* draw_list, ImVec2 minimum, ImVec2 maximum,
                           ImFont* mono);

    /// True when this run has a real CG-residual history, or an adaptive-pass
    /// history that the study actually asked for. Both are plottable; a single
    /// non-adaptive pass is not, so the rail never reserves a cell for nothing.
    bool has_convergence_content() const;

    /// Draws the real residual/ZZ convergence plot filling a dedicated rail
    /// rectangle: a header strip with the newest readout, a left gutter that owns
    /// the y tick labels and a bottom gutter that owns the x-axis caption, so no
    /// text is ever painted over the curve. When both a CG-residual and an
    /// adaptive-pass history exist the rect is split into two labelled plots. It
    /// remains auditable after the worker finishes.
    void draw_convergence_dock(ImDrawList* draw_list, ImVec2 minimum, ImVec2 maximum,
                               ImFont* mono);

    /// Smallest cell height, in device pixels, in which each docked instrument
    /// will actually paint itself. The analysis rail splits on these numbers so
    /// it can never hand a cell to an instrument that then silently declines it
    /// and leaves a hole in the rail.
    float advisor_dock_floor() const;
    float convergence_dock_floor() const;

    /// One-line human-readable summary of what is on screen right now
    /// ("advisor · candidate 29 of 108", "meshing · snap", "solve · pass 2").
    /// Empty when idle. Safe to show in the status bar.
    const char* caption() const;

    /// Drops all queued state and returns to idle.
    void reset();

  private:
    struct Impl;
    Impl* impl_;
};

} // namespace polymesh::gui
