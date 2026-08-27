// SPDX-License-Identifier: BSD-3-Clause

// Honesty invariant: every number and every cell drawn here comes from this run's
// MeshStage, SolveStage, PassTrace, JobProgress, SimSetup or ActivationFrame. Queue
// coalescing is reported from the queue itself. Only wall-clock timing, opacity and
// the reveal front are interpolated; an unavailable network or metric is not drawn.

#include "live_view.hpp"

#include "colormap.hpp"
#include "theme.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace polymesh::gui {
namespace {

constexpr float kRevealShrink = 0.14f;
constexpr float kRevealShrinkFraction = 0.24f;
constexpr float kArrivalBand = 0.045f;
constexpr float kMeshEdgeAlpha = 0.34f;
constexpr std::size_t kDrawnConnections = 180;
constexpr float kNodeRadiusMax = 8.0f;
constexpr std::size_t kMeshQueueCapacity = 3;
constexpr std::size_t kSolveQueueCapacity = 2;
constexpr std::size_t kPassQueueCapacity = 32;
constexpr std::size_t kResidualCapacity = 512;
constexpr std::size_t kPassHistoryCapacity = 64;
// Instrument floors, in dp. The docked advisor needs four resolvable lanes plus
// its candidate strip; the docked convergence card needs its two header rows
// plus one data band. `LiveView::*_dock_floor` publishes them so the analysis
// rail can only ever hand out a cell an instrument will actually paint.
constexpr float kAdvisorCardMargin = 16.0f;
constexpr float kAdvisorLanesFloor = 270.0f;
constexpr float kConvergenceCardInset = 11.0f;
constexpr float kConvergenceCardFloor = 104.0f;
// Smallest data band worth a polyline. Below it the plot is chrome, not an
// instrument, so the caller sheds captions to stay above this line.
constexpr float kPlotDataFloor = 26.0f;
constexpr float kPlotWidthFloor = 56.0f;
constexpr float kUnbounded = 1.0e9f;

using Clock = std::chrono::steady_clock;

float smoothstep(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

float move_toward(float value, float target, float amount) {
    if (value < target) {
        return std::min(value + amount, target);
    }
    return std::max(value - amount, target);
}

ImU32 color(ImVec4 value, float alpha = 1.0f) {
    value.w *= std::clamp(alpha, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(value);
}

ImU32 color(const std::array<float, 3>& value, float alpha = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(
        ImVec4(value[0], value[1], value[2], std::clamp(alpha, 0.0f, 1.0f)));
}

void grouped(char* out, std::size_t capacity, std::size_t value) {
    if (capacity == 0) {
        return;
    }
    std::array<char, 32> digits{};
    const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    if (result.ec != std::errc{}) {
        out[0] = '\0';
        return;
    }
    const std::size_t count = static_cast<std::size_t>(result.ptr - digits.data());
    const std::size_t commas = count > 0 ? (count - 1) / 3 : 0;
    if (count + commas + 1 > capacity) {
        out[0] = '\0';
        return;
    }
    std::size_t dst = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0 && (count - i) % 3 == 0) {
            out[dst++] = ',';
        }
        out[dst++] = digits[i];
    }
    out[dst] = '\0';
}

const std::vector<float>& activation(const advisor::ActivationFrame& frame,
                                     std::size_t layer) {
    if (layer == 0) {
        return frame.input;
    }
    if (layer == 1) {
        return frame.fc1;
    }
    if (layer == 2) {
        return frame.fc2;
    }
    return frame.heads;
}

float percentile(std::vector<float>& values, float q) {
    if (values.empty()) {
        return 1.0f;
    }
    const auto offset = static_cast<std::ptrdiff_t>(q * static_cast<float>(values.size() - 1));
    const auto at = values.begin() + offset;
    std::nth_element(values.begin(), at, values.end());
    return *at > 0.0f ? *at : 1.0f;
}

bool working(pipeline::SolveJob::State state) {
    return state == pipeline::SolveJob::State::kMeshing ||
           state == pipeline::SolveJob::State::kSolving;
}

bool terminal(pipeline::SolveJob::State state) {
    return state == pipeline::SolveJob::State::kDone ||
           state == pipeline::SolveJob::State::kFailed ||
           state == pipeline::SolveJob::State::kMeshDone ||
           state == pipeline::SolveJob::State::kCancelled;
}

} // namespace

struct LiveView::Impl {
    struct PendingMesh {
        pipeline::MeshStage stage;
        Clock::time_point arrival;
    };

    struct EdgePick {
        float rank = 0.0f;
        float value = 0.0f;
        int block = 0;
        int source = 0;
        int destination = 0;
    };

    struct ResidualSample {
        int iteration = 0;
        double residual = 0.0;
    };

    std::mutex queue_mutex;
    std::deque<PendingMesh> pending_meshes;
    std::deque<pipeline::SolveStage> pending_solves;
    std::deque<pipeline::PassTrace> pending_passes;
    std::deque<PendingMesh> drained_meshes;
    std::deque<pipeline::SolveStage> drained_solves;
    std::deque<pipeline::PassTrace> drained_passes;
    std::size_t dropped_meshes = 0;
    std::size_t skipped_meshes = 0;
    std::size_t coalesced_meshes = 0;

    pipeline::SolveJob* attached_job = nullptr;
    bool run_active = false;
    bool worker_finished = false;
    bool result_landed = false;
    bool mesh_building = false;
    bool solving = false;
    bool owns_canvas = false;

    std::optional<pipeline::SimSetup> setup;
    std::optional<pipeline::MeshStage> current_stage;
    std::optional<Clock::time_point> last_stage_arrival;
    pipeline::JobProgress progress;
    bool progress_valid = false;

    float reveal = 0.0f;
    float reveal_duration = 0.65f;
    bool transition = false;
    float stage_alpha = 0.0f;
    float advisor_alpha = 0.0f;
    float convergence_alpha = 0.0f;

    std::optional<advisor::AdvisorExplanation> explanation;
    std::optional<advisor::NetworkLayout> layout;
    bool advisor_ready = false;
    std::array<float, 4> layer_scale{1.0f, 1.0f, 1.0f, 1.0f};
    float contribution_scale = 1.0f;
    float score_min = 0.0f;
    float score_max = 0.0f;
    bool score_range = false;
    std::vector<std::size_t> candidate_frames;
    int winner_candidate_slot = -1;
    std::vector<int> chosen_heads;
    float advisor_cursor = 0.0f;
    float advisor_frame_period = 0.055f;
    std::size_t advisor_frame = 0;
    std::vector<EdgePick> edge_scratch;

    std::array<ResidualSample, kResidualCapacity> residuals{};
    std::size_t residual_head = 0;
    std::size_t residual_count = 0;
    int residual_pass = -1;
    std::array<pipeline::PassTrace, kPassHistoryCapacity> pass_history{};
    std::size_t pass_history_count = 0;

    std::string caption_storage;

    void clear_runtime() {
        {
            const std::lock_guard<std::mutex> lock(queue_mutex);
            pending_meshes.clear();
            pending_solves.clear();
            pending_passes.clear();
            dropped_meshes = 0;
        }
        drained_meshes.clear();
        drained_solves.clear();
        drained_passes.clear();
        skipped_meshes = 0;
        coalesced_meshes = 0;
        worker_finished = false;
        result_landed = false;
        mesh_building = false;
        solving = false;
        owns_canvas = false;
        current_stage.reset();
        last_stage_arrival.reset();
        progress = {};
        progress_valid = false;
        reveal = 0.0f;
        reveal_duration = 0.65f;
        transition = false;
        stage_alpha = 0.0f;
        advisor_alpha = 0.0f;
        convergence_alpha = 0.0f;
        advisor_cursor = 0.0f;
        advisor_frame = 0;
        residual_head = 0;
        residual_count = 0;
        residual_pass = -1;
        pass_history_count = 0;
        caption_storage.clear();
    }

    void clear_all() {
        clear_runtime();
        setup.reset();
        explanation.reset();
        layout.reset();
        advisor_ready = false;
        candidate_frames.clear();
        chosen_heads.clear();
        edge_scratch.clear();
        winner_candidate_slot = -1;
        run_active = false;
    }

    void enqueue_mesh(const pipeline::MeshStage& stage) {
        PendingMesh pending{stage, Clock::now()};
        const std::lock_guard<std::mutex> lock(queue_mutex);
        if (pending_meshes.size() == kMeshQueueCapacity) {
            pending_meshes.pop_front();
            ++dropped_meshes;
        }
        pending_meshes.push_back(std::move(pending));
    }

    void enqueue_solve(const pipeline::SolveStage& stage) {
        pipeline::SolveStage pending = stage;
        const std::lock_guard<std::mutex> lock(queue_mutex);
        if (pending_solves.size() == kSolveQueueCapacity) {
            pending_solves.pop_front();
        }
        pending_solves.push_back(std::move(pending));
    }

    void enqueue_pass(const pipeline::PassTrace& trace) {
        pipeline::PassTrace pending = trace;
        const std::lock_guard<std::mutex> lock(queue_mutex);
        if (pending_passes.size() == kPassQueueCapacity) {
            pending_passes.pop_front();
        }
        pending_passes.push_back(std::move(pending));
    }

    void add_pass(const pipeline::PassTrace& trace) {
        for (std::size_t i = 0; i < pass_history_count; ++i) {
            if (pass_history[i].pass == trace.pass) {
                pass_history[i] = trace;
                return;
            }
        }
        if (pass_history_count < pass_history.size()) {
            pass_history[pass_history_count++] = trace;
            return;
        }
        for (std::size_t i = 1; i < pass_history.size(); ++i) {
            pass_history[i - 1] = std::move(pass_history[i]);
        }
        pass_history.back() = trace;
    }

    void add_residual(const pipeline::JobProgress& sample) {
        if (sample.cg_iter < 0 || !(sample.cg_resid > 0.0) ||
            !std::isfinite(sample.cg_resid)) {
            return;
        }
        // A pass change, or an iteration counter that walks backwards, means a
        // different linear system: the previous trace belongs to a matrix that no
        // longer exists. Keeping it drew two superimposed curves joined by a
        // meaningless diagonal, because x is the CG iteration and the new solve
        // restarts at zero.
        if (residual_pass != sample.pass) {
            residual_head = 0;
            residual_count = 0;
            residual_pass = sample.pass;
        }
        if (residual_count > 0) {
            const auto& last = residual_at(residual_count - 1);
            if (last.iteration == sample.cg_iter && last.residual == sample.cg_resid) {
                return;
            }
            if (sample.cg_iter < last.iteration) {
                residual_head = 0;
                residual_count = 0;
            }
        }
        const std::size_t slot = (residual_head + residual_count) % residuals.size();
        if (residual_count < residuals.size()) {
            residuals[slot] = {sample.cg_iter, sample.cg_resid};
            ++residual_count;
        } else {
            residuals[residual_head] = {sample.cg_iter, sample.cg_resid};
            residual_head = (residual_head + 1) % residuals.size();
        }
    }

    const ResidualSample& residual_at(std::size_t index) const {
        return residuals[(residual_head + index) % residuals.size()];
    }

    void prepare_advisor() {
        advisor_ready = false;
        candidate_frames.clear();
        chosen_heads.clear();
        edge_scratch.clear();
        winner_candidate_slot = -1;
        layer_scale = {1.0f, 1.0f, 1.0f, 1.0f};
        contribution_scale = 1.0f;
        score_min = 0.0f;
        score_max = 0.0f;
        score_range = false;
        advisor_cursor = 0.0f;
        advisor_frame = 0;
        advisor_frame_period = 0.055f;

        if (!explanation || !layout || explanation->frames.empty() ||
            layout->layers.size() != 4 || layout->edges.size() != 3) {
            return;
        }
        for (std::size_t layer = 0; layer < 4; ++layer) {
            for (const auto& frame : explanation->frames) {
                if (activation(frame, layer).size() != layout->layers[layer].size) {
                    return;
                }
            }
        }

        std::size_t total_connections = 0;
        for (std::size_t block_index = 0; block_index < layout->edges.size(); ++block_index) {
            const auto& block = layout->edges[block_index];
            const auto& source = layout->layers[block_index];
            const auto& destination = layout->layers[block_index + 1];
            if (block.from != source.name || block.to != destination.name ||
                block.cols != source.size || block.rows != destination.size ||
                block.weights.size() != block.rows * block.cols) {
                return;
            }
            total_connections += block.rows * block.cols;
        }

        std::vector<float> pool;
        for (std::size_t layer = 0; layer < 4; ++layer) {
            pool.clear();
            pool.reserve(explanation->frames.size() * layout->layers[layer].size);
            for (const auto& frame : explanation->frames) {
                for (const float value : activation(frame, layer)) {
                    pool.push_back(std::fabs(value));
                }
            }
            layer_scale[layer] = percentile(pool, 0.98f);
        }

        pool.clear();
        pool.reserve(explanation->frames.size() * total_connections);
        for (const auto& frame : explanation->frames) {
            for (std::size_t block_index = 0; block_index < layout->edges.size();
                 ++block_index) {
                const auto& block = layout->edges[block_index];
                const auto& source = activation(frame, block_index);
                for (std::size_t destination = 0; destination < block.rows; ++destination) {
                    const float* row = block.weights.data() + destination * block.cols;
                    for (std::size_t input = 0; input < block.cols; ++input) {
                        pool.push_back(std::fabs(row[input] * source[input]));
                    }
                }
            }
        }
        contribution_scale = percentile(pool, 0.98f);
        edge_scratch.reserve(total_connections);
        candidate_frames.reserve(explanation->frames.size());
        chosen_heads.reserve(explanation->frames.size());
        for (const auto& frame : explanation->frames) {
            const std::string wanted =
                std::string("policy_mesher_logit_") + frame.action.mesher;
            int chosen = -1;
            for (std::size_t i = 0; i < layout->layers[3].labels.size(); ++i) {
                if (layout->layers[3].labels[i] == wanted) {
                    chosen = static_cast<int>(i);
                    break;
                }
            }
            chosen_heads.push_back(chosen);
        }

        bool any_score = false;
        for (std::size_t i = 0; i < explanation->frames.size(); ++i) {
            const auto& frame = explanation->frames[i];
            if (frame.candidate >= 0) {
                candidate_frames.push_back(i);
            }
            if (frame.ranked && std::isfinite(frame.score)) {
                const float score = static_cast<float>(frame.score);
                score_min = any_score ? std::min(score_min, score) : score;
                score_max = any_score ? std::max(score_max, score) : score;
                any_score = true;
            }
        }
        score_range = any_score && score_max > score_min;

        const advisor::ActivationFrame* recommended = nullptr;
        for (const auto& frame : explanation->frames) {
            if (frame.recommended) {
                recommended = &frame;
            }
        }
        if (recommended != nullptr) {
            const auto& wanted = recommended->action;
            for (std::size_t slot = 0; slot < candidate_frames.size(); ++slot) {
                const auto& frame = explanation->frames[candidate_frames[slot]];
                if (frame.action.mesher == wanted.mesher &&
                    frame.action.order == wanted.order &&
                    frame.action.adapt_passes == wanted.adapt_passes &&
                    std::fabs(frame.action.h_rel - wanted.h_rel) < 1.0e-9 &&
                    std::fabs(frame.action.eta_target - wanted.eta_target) < 1.0e-9) {
                    winner_candidate_slot = static_cast<int>(slot);
                    break;
                }
            }
        }
        advisor_ready = true;
    }

    void update_caption() {
        caption_storage.clear();
        char line[160]{};
        if (mesh_building && advisor_alpha > 0.05f && advisor_ready && explanation &&
            advisor_frame < explanation->frames.size()) {
            const auto& frame = explanation->frames[advisor_frame];
            if (frame.candidate >= 0) {
                std::snprintf(line, sizeof(line), "advisor - candidate %d of %zu",
                              frame.candidate + 1, candidate_frames.size());
            } else {
                std::snprintf(line, sizeof(line), "advisor - final re-score");
            }
            caption_storage = line;
            return;
        }
        if (current_stage && (mesh_building || owns_canvas)) {
            char cells[40]{};
            grouped(cells, sizeof(cells), current_stage->mesh.elements.size());
            std::snprintf(line, sizeof(line), "meshing - %s - %s cells",
                          current_stage->stage.c_str(), cells);
            caption_storage = line;
            return;
        }
        if (solving || convergence_alpha > 0.05f) {
            if (progress.cg_iter > 0) {
                std::snprintf(line, sizeof(line), "solve - pass %d - CG %d", progress.pass + 1,
                              progress.cg_iter);
            } else {
                std::snprintf(line, sizeof(line), "solve - pass %d", progress.pass + 1);
            }
            caption_storage = line;
        }
    }

    void draw_stage_hud(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImFont* mono) const {
        if (stage_alpha <= 0.001f || !current_stage) {
            return;
        }
        const float margin = ui_px(16.0f);
        const float available_width = mx.x - mn.x - 2.0f * margin;
        if (available_width < ui_px(220.0f)) {
            return;
        }
        const float width = std::min(ui_px(340.0f), available_width);
        const float height = ui_px(coalesced_meshes > 0 ? 124.0f : 104.0f);
        const ImVec2 card_min(mn.x + margin, mx.y - margin - height);
        const ImVec2 card_max(card_min.x + width, card_min.y + height);
        glass_background(dl, card_min, card_max, 9.0f, stage_alpha);

        ImFont* body = ImGui::GetFont();
        ImFont* numbers = mono != nullptr ? mono : body;
        const float body_size = body->FontSize;
        const float number_size = numbers->FontSize;
        const float x = card_min.x + ui_px(15.0f);
        float y = card_min.y + ui_px(13.0f);
        dl->AddText(body, body_size, ImVec2(x, y), color(palette.accent, stage_alpha),
                    current_stage->stage.c_str());
        y += ui_px(25.0f);

        char elems[40]{};
        char nodes[40]{};
        char line[192]{};
        grouped(elems, sizeof(elems), current_stage->mesh.elements.size());
        grouped(nodes, sizeof(nodes), current_stage->mesh.nodes.size());
        std::snprintf(line, sizeof(line), "%s cells   %s nodes", elems, nodes);
        dl->AddText(numbers, number_size, ImVec2(x, y), color(palette.text, stage_alpha),
                    line);
        y += ui_px(23.0f);

        const int pass_total = progress_valid
                                   ? std::max(progress.pass_count + 1, 1)
                                   : std::max(setup ? setup->adapt_passes + 1 : 1, 1);
        const double elapsed_ms = progress_valid ? progress.elapsed_ms : 0.0;
        std::snprintf(line, sizeof(line), "pass %d of %d   elapsed %.2f s",
                      current_stage->pass + 1, pass_total, elapsed_ms / 1000.0);
        dl->AddText(numbers, number_size, ImVec2(x, y), color(palette.text_dim, stage_alpha),
                    line);
        if (coalesced_meshes > 0) {
            y += ui_px(21.0f);
            std::snprintf(line, sizeof(line), "%zu stage%s coalesced", coalesced_meshes,
                          coalesced_meshes == 1 ? "" : "s");
            dl->AddText(numbers, number_size, ImVec2(x, y),
                        color(palette.status_warn, stage_alpha), line);
        }
    }

    void rank_edges(const advisor::ActivationFrame& frame) {
        edge_scratch.clear();
        if (!layout) {
            return;
        }
        for (std::size_t block_index = 0; block_index < layout->edges.size(); ++block_index) {
            const auto& block = layout->edges[block_index];
            const auto& source = activation(frame, block_index);
            for (std::size_t destination = 0; destination < block.rows; ++destination) {
                const float* row = block.weights.data() + destination * block.cols;
                for (std::size_t input = 0; input < block.cols; ++input) {
                    const float value = row[input] * source[input];
                    edge_scratch.push_back(
                        {std::fabs(value), value, static_cast<int>(block_index),
                         static_cast<int>(input), static_cast<int>(destination)});
                }
            }
        }
        const std::size_t count = std::min(kDrawnConnections, edge_scratch.size());
        if (count == 0) {
            return;
        }
        std::nth_element(edge_scratch.begin(),
                         edge_scratch.begin() + static_cast<std::ptrdiff_t>(count - 1),
                         edge_scratch.end(),
                         [](const EdgePick& a, const EdgePick& b) { return a.rank > b.rank; });
        std::sort(edge_scratch.begin(),
                  edge_scratch.begin() + static_cast<std::ptrdiff_t>(count),
                  [](const EdgePick& a, const EdgePick& b) { return a.rank < b.rank; });
    }

    void draw_advisor(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImFont* mono, bool docked) {
        if (advisor_alpha <= 0.001f || !advisor_ready || !explanation || !layout ||
            advisor_frame >= explanation->frames.size()) {
            return;
        }
        const auto& frame = explanation->frames[advisor_frame];

        const float margin = ui_px(kAdvisorCardMargin);
        // The viewport's `frame (F)` action owns the upper-right corner. The
        // advisor used to start at the same y and the button sat on top of the
        // score, visibly clipping the instrumentation we were trying to make
        // legible. Reserve its row before fitting the card, just as a normal
        // layout would reserve chrome before content.
        const float top_chrome = docked ? 0.0f : ui_px(38.0f);
        const float available_width = mx.x - mn.x - 2.0f * margin;
        const float available_height = mx.y - mn.y - 2.0f * margin - top_chrome;
        const float minimum_width = ui_px(docked ? 240.0f : 300.0f);
        if (available_width < minimum_width || available_height < ui_px(kAdvisorLanesFloor)) {
            return;
        }
        rank_edges(frame);
        // Large enough to resolve all four real layers and 108 candidate
        // markers, but no longer half the model. The film is allowed to own a
        // whole act; live instrumentation is a guest over the user's part.
        const float width =
            docked ? available_width
                   : std::min(ui_px(560.0f), std::max(ui_px(300.0f), available_width * 0.48f));
        const float height =
            docked ? available_height : std::min(ui_px(365.0f), available_height);
        const ImVec2 card_min(mx.x - margin - width, mn.y + margin + top_chrome);
        const ImVec2 card_max(card_min.x + width, card_min.y + height);
        glass_background(dl, card_min, card_max, 9.0f, advisor_alpha);

        ImFont* body = ImGui::GetFont();
        ImFont* numbers = mono != nullptr ? mono : body;
        const float body_size = body->FontSize;
        const float number_size = numbers->FontSize;
        const float pad = ui_px(15.0f);
        const float inner_x0 = card_min.x + pad;
        const float inner_x1 = card_max.x - pad;
        const float label_width = ui_px(58.0f);
        const float nodes_x0 = inner_x0 + label_width;
        const float nodes_x1 = inner_x1;

        dl->AddText(body, body_size, ImVec2(inner_x0, card_min.y + ui_px(12.0f)),
                    color(palette.accent, advisor_alpha), "advisor activation");
        char header[128]{};
        const char* prefix = frame.candidate >= 0 ? "candidate" : "final re-score";
        if (frame.ranked && std::isfinite(frame.score)) {
            if (frame.candidate >= 0) {
                std::snprintf(header, sizeof(header), "%s %d / %zu   score %.5g", prefix,
                              frame.candidate + 1, candidate_frames.size(), frame.score);
            } else {
                std::snprintf(header, sizeof(header), "%s   score %.5g", prefix, frame.score);
            }
        } else if (frame.candidate >= 0) {
            std::snprintf(header, sizeof(header), "%s %d / %zu   score unavailable", prefix,
                          frame.candidate + 1, candidate_frames.size());
        } else {
            std::snprintf(header, sizeof(header), "%s   score unavailable", prefix);
        }
        const float header_width =
            numbers
                ->CalcTextSizeA(number_size, std::numeric_limits<float>::max(), 0.0f, header)
                .x;
        const float header_y = card_min.y + ui_px(docked ? 31.0f : 12.0f);
        const float header_x = docked ? inner_x0 : std::max(inner_x0, inner_x1 - header_width);
        dl->AddText(numbers, number_size, ImVec2(header_x, header_y),
                    color(palette.text_dim, advisor_alpha), header);

        const float lanes_top = card_min.y + ui_px(docked ? 61.0f : 43.0f);
        const float strip_height = ui_px(82.0f);
        const float lanes_bottom = card_max.y - pad - strip_height;
        const float lane_height = (lanes_bottom - lanes_top) / 4.0f;
        const std::array<const char*, 4> lane_names{"input", "fc1", "fc2", "heads"};
        const auto lane_y = [&](std::size_t layer) {
            return lanes_top + lane_height * (static_cast<float>(layer) + 0.58f);
        };
        const auto node_x = [&](std::size_t layer, std::size_t node) {
            const std::size_t count = std::max<std::size_t>(layout->layers[layer].size, 1);
            return nodes_x0 + (nodes_x1 - nodes_x0) * (static_cast<float>(node) + 0.5f) /
                                  static_cast<float>(count);
        };
        const auto node_point = [&](std::size_t layer, std::size_t node) {
            return ImVec2(node_x(layer, node), lane_y(layer));
        };

        for (std::size_t layer = 0; layer < 4; ++layer) {
            const float top =
                lanes_top + lane_height * static_cast<float>(layer) + ui_px(3.0f);
            const float bottom =
                lanes_top + lane_height * static_cast<float>(layer + 1) - ui_px(3.0f);
            dl->AddRectFilled(ImVec2(inner_x0, top), ImVec2(inner_x1, bottom),
                              color(layer == 3 ? palette.accent_soft : palette.surface_hi,
                                    advisor_alpha * (layer == 3 ? 0.32f : 0.20f)),
                              ui_px(5.0f));
            dl->AddLine(
                ImVec2(nodes_x0, lane_y(layer)), ImVec2(nodes_x1, lane_y(layer)),
                color(layer == 3 ? palette.accent : palette.text_dim, advisor_alpha * 0.22f),
                ui_px(1.0f));
            dl->AddText(body, body_size, ImVec2(inner_x0, top + ui_px(3.0f)),
                        color(layer == 3 ? palette.accent : palette.text_dim, advisor_alpha),
                        lane_names[layer]);
        }

        const std::size_t drawn = std::min(kDrawnConnections, edge_scratch.size());
        const float inverse_contribution =
            contribution_scale > 0.0f ? 1.0f / contribution_scale : 0.0f;
        for (std::size_t i = 0; i < drawn; ++i) {
            const auto& pick = edge_scratch[i];
            const float strength = std::clamp(pick.rank * inverse_contribution, 0.0f, 1.0f);
            const float signed_strength =
                (pick.value < 0.0f ? -1.0f : 1.0f) * (0.34f + 0.66f * strength);
            const std::size_t block = static_cast<std::size_t>(pick.block);
            dl->AddLine(node_point(block, static_cast<std::size_t>(pick.source)),
                        node_point(block + 1, static_cast<std::size_t>(pick.destination)),
                        color(signed_colormap(signed_strength),
                              advisor_alpha * (0.06f + 0.84f * strength)),
                        ui_px(0.55f + 1.55f * strength));
        }

        const int chosen_head =
            advisor_frame < chosen_heads.size() ? chosen_heads[advisor_frame] : -1;
        for (std::size_t layer = 0; layer < 4; ++layer) {
            const auto& values = activation(frame, layer);
            if (values.empty()) {
                continue;
            }
            const float spacing = (nodes_x1 - nodes_x0) / static_cast<float>(values.size());
            const float maximum_radius = std::min(ui_px(kNodeRadiusMax), 0.44f * spacing);
            const float minimum_radius = std::min(ui_px(1.6f), maximum_radius);
            for (std::size_t i = 0; i < values.size(); ++i) {
                const float normalized =
                    std::clamp(values[i] / layer_scale[layer], -1.0f, 1.0f);
                const float magnitude = std::fabs(normalized);
                const float radius =
                    minimum_radius + (maximum_radius - minimum_radius) * magnitude;
                const ImVec2 point = node_point(layer, i);
                const auto node_color = signed_colormap(normalized);
                if (magnitude > 0.30f) {
                    dl->AddCircleFilled(point, radius * 2.4f,
                                        color(node_color, advisor_alpha * 0.08f * magnitude));
                }
                dl->AddCircleFilled(
                    point, radius,
                    color(node_color, advisor_alpha * (0.42f + 0.58f * magnitude)));
                const bool chosen = layer == 3 && static_cast<int>(i) == chosen_head;
                if (chosen) {
                    dl->AddCircle(point, radius + ui_px(3.0f),
                                  color(palette.accent, advisor_alpha), 0, ui_px(2.0f));
                    dl->AddCircle(point, radius + ui_px(6.0f),
                                  color(palette.accent_soft, advisor_alpha * 0.55f), 0,
                                  ui_px(1.2f));
                }
            }
        }

        const float strip_top = lanes_bottom + ui_px(8.0f);
        const float plot_top = strip_top + ui_px(22.0f);
        const float plot_bottom = card_max.y - pad;
        dl->AddText(body, body_size, ImVec2(inner_x0, strip_top),
                    color(palette.text_dim, advisor_alpha),
                    "candidate score - lower is better");
        if (!candidate_frames.empty()) {
            const float plot_x0 = nodes_x0;
            const float plot_x1 = nodes_x1;
            dl->AddLine(ImVec2(plot_x0, plot_bottom), ImVec2(plot_x1, plot_bottom),
                        color(palette.border, advisor_alpha), ui_px(1.0f));
            for (std::size_t slot = 0; slot < candidate_frames.size(); ++slot) {
                const auto& candidate = explanation->frames[candidate_frames[slot]];
                const float x = plot_x0 + (plot_x1 - plot_x0) *
                                              (static_cast<float>(slot) + 0.5f) /
                                              static_cast<float>(candidate_frames.size());
                float y = plot_bottom;
                if (candidate.ranked && std::isfinite(candidate.score) && score_range) {
                    const float fraction = (static_cast<float>(candidate.score) - score_min) /
                                           (score_max - score_min);
                    y = plot_bottom -
                        std::clamp(fraction, 0.0f, 1.0f) * (plot_bottom - plot_top);
                }
                const ImVec4 marker =
                    candidate.over_budget
                        ? palette.status_err
                        : (candidate.gate_pass ? palette.status_ok : palette.status_warn);
                if (candidate.gate_pass && !candidate.over_budget) {
                    dl->AddCircleFilled(ImVec2(x, y), ui_px(2.7f),
                                        color(marker, advisor_alpha * 0.88f));
                } else {
                    dl->AddCircle(ImVec2(x, y), ui_px(2.7f),
                                  color(marker, advisor_alpha * 0.82f), 0, ui_px(1.2f));
                }
                if (static_cast<int>(slot) == winner_candidate_slot) {
                    dl->AddCircle(ImVec2(x, y), ui_px(7.0f),
                                  color(palette.accent, advisor_alpha), 0, ui_px(1.8f));
                }
            }
        }
    }

    struct PlotFrame {
        float gutter_x0 = 0.0f;
        float x0 = 0.0f;
        float x1 = 0.0f;
        float top = 0.0f;
        float bottom = 0.0f;
        float tick_right = 0.0f;
        /// Ticks that fit without one label landing on the next.
        int tick_capacity = 0;
        bool valid = false;
    };

    // Reserves the two gutters that keep axis text out of the data area: a left
    // gutter that owns every y tick label and a bottom strip that owns the x
    // caption. The returned rect is where the polyline, its fill and its markers
    // are allowed to live, so a curve can never run underneath a number.
    //
    // `widest_tick` is the widest label the caller will hand to `draw_y_tick`.
    // The gutter is measured from it instead of guessed: at this font a fixed
    // 58 dp was narrower than "2.0e+00", so the number spilled into the data
    // rect and sat on the residual curve it was labelling.
    PlotFrame frame_plot(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImFont* body, ImFont* numbers,
                         const char* caption, const char* readout, const char* x_caption,
                         const char* widest_tick) const {
        const float body_size = body->FontSize;
        const float number_size = numbers->FontSize;
        const float tick_width =
            numbers->CalcTextSizeA(number_size, kUnbounded, 0.0f, widest_tick).x;
        const float gutter = std::min(tick_width + ui_px(8.0f), 0.46f * (mx.x - mn.x));
        PlotFrame frame;
        frame.gutter_x0 = mn.x;
        frame.x0 = mn.x + gutter;
        frame.x1 = mx.x;
        frame.tick_right = frame.x0 - ui_px(6.0f);
        if (frame.x1 - frame.x0 < ui_px(kPlotWidthFloor)) {
            return frame;
        }

        // Chrome is shed before the data band. The polyline is the instrument; a
        // caption and an axis name are labels for it, and a labelled empty box
        // tells the user nothing about their solve.
        const float caption_height = std::max(body_size, number_size) + ui_px(7.0f);
        const float axis_height = body_size + ui_px(6.0f);
        const float height = mx.y - mn.y;
        bool with_caption = caption != nullptr;
        bool with_axis = x_caption != nullptr;
        if (with_caption && height - caption_height - axis_height < ui_px(kPlotDataFloor)) {
            with_caption = false;
        }
        if (with_axis && height - (with_caption ? caption_height : 0.0f) - axis_height <
                             ui_px(kPlotDataFloor)) {
            with_axis = false;
        }
        frame.top = mn.y + (with_caption ? caption_height : 0.0f);
        frame.bottom = mx.y - (with_axis ? axis_height : ui_px(1.0f));
        if (frame.bottom - frame.top < ui_px(kPlotDataFloor)) {
            return frame;
        }
        frame.valid = true;
        frame.tick_capacity = std::clamp(
            static_cast<int>((frame.bottom - frame.top) / (number_size * 1.9f)), 1, 4);

        if (with_caption) {
            dl->AddText(body, body_size, ImVec2(mn.x, mn.y),
                        color(palette.text_dim, convergence_alpha), caption);
            if (readout != nullptr && readout[0] != '\0') {
                const float width =
                    numbers->CalcTextSizeA(number_size, kUnbounded, 0.0f, readout).x;
                dl->AddText(numbers, number_size, ImVec2(std::max(mn.x, mx.x - width), mn.y),
                            color(palette.text, convergence_alpha), readout);
            }
        }
        dl->AddLine(ImVec2(frame.x0, frame.top), ImVec2(frame.x0, frame.bottom),
                    color(palette.border, convergence_alpha), ui_px(1.0f));
        dl->AddLine(ImVec2(frame.x0, frame.bottom), ImVec2(frame.x1, frame.bottom),
                    color(palette.border, convergence_alpha), ui_px(1.0f));
        if (with_axis) {
            dl->AddText(body, body_size, ImVec2(frame.x0, frame.bottom + ui_px(3.0f)),
                        color(palette.text_dim, convergence_alpha), x_caption);
        }
        return frame;
    }

    void draw_y_tick(ImDrawList* dl, ImFont* numbers, const PlotFrame& frame, float y,
                     const char* text) const {
        const float number_size = numbers->FontSize;
        dl->AddLine(ImVec2(frame.x0, y), ImVec2(frame.x1, y),
                    color(palette.border, convergence_alpha * 0.5f), ui_px(1.0f));
        const float width = numbers->CalcTextSizeA(number_size, kUnbounded, 0.0f, text).x;
        const float text_y =
            std::clamp(y - 0.5f * number_size, frame.top, frame.bottom - number_size);
        // The tick column is clipped, not merely measured: a label wider than the
        // gutter loses its leading digits rather than crossing into the curve.
        dl->PushClipRect(ImVec2(frame.gutter_x0, frame.top),
                         ImVec2(frame.tick_right, frame.bottom), true);
        dl->AddText(numbers, number_size, ImVec2(frame.tick_right - width, text_y),
                    color(palette.text_dim, convergence_alpha), text);
        dl->PopClipRect();
    }

    void draw_residual_plot(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImFont* body,
                            ImFont* numbers, const char* caption, const char* readout) const {
        double log_min = std::numeric_limits<double>::infinity();
        double log_max = -std::numeric_limits<double>::infinity();
        int iteration_min = residual_at(0).iteration;
        int iteration_max = iteration_min;
        for (std::size_t i = 0; i < residual_count; ++i) {
            const auto& sample = residual_at(i);
            iteration_min = std::min(iteration_min, sample.iteration);
            iteration_max = std::max(iteration_max, sample.iteration);
            if (!(sample.residual > 0.0)) {
                continue;
            }
            const double value = std::log10(sample.residual);
            log_min = std::min(log_min, value);
            log_max = std::max(log_max, value);
        }
        if (!std::isfinite(log_min) || !std::isfinite(log_max)) {
            return;
        }
        // "%.0e" instead of "%.1e": one significant digit is all a decade tick
        // needs, and every character in this label is charged to the gutter.
        char tick[32]{};
        char widest[32]{};
        std::snprintf(tick, sizeof(tick), "%.0e", std::pow(10.0, log_min));
        std::snprintf(widest, sizeof(widest), "%.0e", std::pow(10.0, log_max));
        if (std::strlen(tick) > std::strlen(widest)) {
            std::memcpy(widest, tick, sizeof(widest));
        }
        const PlotFrame frame =
            frame_plot(dl, mn, mx, body, numbers, caption, readout, "CG iterations", widest);
        if (!frame.valid) {
            return;
        }
        const bool flat = !(log_max - log_min > 1.0e-6);
        const double log_span = std::max(log_max - log_min, 1.0e-12);
        const int iteration_span = std::max(iteration_max - iteration_min, 1);
        const float height = frame.bottom - frame.top;
        const auto y_of = [&](double residual) {
            if (flat) {
                return 0.5f * (frame.top + frame.bottom);
            }
            const double value = residual > 0.0 ? std::log10(residual) : log_min;
            const float fraction = static_cast<float>((value - log_min) / log_span);
            return frame.bottom - std::clamp(fraction, 0.0f, 1.0f) * height;
        };

        const int tick_count = flat ? 1 : frame.tick_capacity;
        const double tick_step =
            tick_count > 1 ? log_span / static_cast<double>(tick_count - 1) : 0.0;
        for (int i = 0; i < tick_count; ++i) {
            const double value = std::pow(10.0, log_min + tick_step * static_cast<double>(i));
            std::snprintf(tick, sizeof(tick), "%.0e", value);
            draw_y_tick(dl, numbers, frame, y_of(value), tick);
        }

        // The curve is clipped to its own rect, so no fill, segment or marker can
        // reach back into the tick column. The marker radius is the only slack:
        // the newest sample sits exactly on the right edge.
        const float marker = ui_px(3.0f);
        dl->PushClipRect(ImVec2(frame.x0, frame.top - marker),
                         ImVec2(frame.x1 + marker, frame.bottom + marker), true);
        ImVec2 previous{};
        for (std::size_t i = 0; i < residual_count; ++i) {
            const auto& sample = residual_at(i);
            const float x =
                frame.x0 + (frame.x1 - frame.x0) *
                               static_cast<float>(sample.iteration - iteration_min) /
                               static_cast<float>(iteration_span);
            const ImVec2 point(x, y_of(sample.residual));
            if (i > 0) {
                if (point.x > previous.x) {
                    dl->AddQuadFilled(previous, point, ImVec2(point.x, frame.bottom),
                                      ImVec2(previous.x, frame.bottom),
                                      color(palette.accent2, convergence_alpha * 0.13f));
                }
                dl->AddLine(previous, point, color(palette.accent2, convergence_alpha),
                            ui_px(1.7f));
            }
            previous = point;
        }
        dl->AddCircleFilled(previous, marker, color(palette.accent2, convergence_alpha));
        dl->PopClipRect();
    }

    void draw_eta_plot(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImFont* body, ImFont* numbers,
                       const char* caption, const char* readout) const {
        double eta_max = 0.0;
        for (std::size_t i = 0; i < pass_history_count; ++i) {
            eta_max = std::max(eta_max, pass_history[i].global_eta);
        }
        const bool target_active = setup && setup->eta_target > 0.0;
        if (target_active) {
            eta_max = std::max(eta_max, setup->eta_target);
        }
        if (!(eta_max > 0.0)) {
            return;
        }
        // "%.2g" keeps the gutter narrow: an eta tick is a fraction between 0
        // and the largest of the history and the target, so two digits resolve
        // every tick this axis can produce.
        char tick[32]{};
        char widest[32]{};
        std::snprintf(widest, sizeof(widest), "%.2g", eta_max);
        std::snprintf(tick, sizeof(tick), "%.2g", eta_max / 3.0);
        if (std::strlen(tick) > std::strlen(widest)) {
            std::memcpy(widest, tick, sizeof(widest));
        }
        const PlotFrame frame =
            frame_plot(dl, mn, mx, body, numbers, caption, readout, "adaptive pass", widest);
        if (!frame.valid) {
            return;
        }
        const float height = frame.bottom - frame.top;
        const auto y_of = [&](double eta) {
            const float fraction = static_cast<float>(eta / eta_max);
            return frame.bottom - std::clamp(fraction, 0.0f, 1.0f) * height;
        };

        for (int i = 0; i < frame.tick_capacity; ++i) {
            const double value = frame.tick_capacity > 1
                                     ? eta_max * static_cast<double>(i) /
                                           static_cast<double>(frame.tick_capacity - 1)
                                     : eta_max;
            std::snprintf(tick, sizeof(tick), "%.2g", value);
            draw_y_tick(dl, numbers, frame, y_of(value), tick);
        }
        if (target_active) {
            const float y = y_of(setup->eta_target);
            dl->AddLine(ImVec2(frame.x0, y), ImVec2(frame.x1, y),
                        color(palette.status_warn, convergence_alpha * 0.82f), ui_px(1.2f));
        }

        const float marker = ui_px(3.0f);
        dl->PushClipRect(ImVec2(frame.x0, frame.top - marker),
                         ImVec2(frame.x1 + marker, frame.bottom + marker), true);
        ImVec2 previous{};
        for (std::size_t i = 0; i < pass_history_count; ++i) {
            const float x = pass_history_count == 1
                                ? 0.5f * (frame.x0 + frame.x1)
                                : frame.x0 + (frame.x1 - frame.x0) * static_cast<float>(i) /
                                                 static_cast<float>(pass_history_count - 1);
            const ImVec2 point(x, y_of(pass_history[i].global_eta));
            if (i > 0) {
                if (point.x > previous.x) {
                    dl->AddQuadFilled(previous, point, ImVec2(point.x, frame.bottom),
                                      ImVec2(previous.x, frame.bottom),
                                      color(palette.accent2, convergence_alpha * 0.13f));
                }
                dl->AddLine(previous, point, color(palette.accent2, convergence_alpha),
                            ui_px(1.7f));
            }
            dl->AddCircleFilled(point, marker, color(palette.accent2, convergence_alpha));
            previous = point;
        }
        dl->PopClipRect();
    }

    void draw_convergence(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImFont* mono,
                          bool docked) const {
        const bool has_eta = setup && setup->adapt_passes > 0 && pass_history_count > 0;
        const bool has_residual = residual_count > 0;
        if (convergence_alpha <= 0.001f || (!has_residual && !has_eta)) {
            return;
        }

        ImVec2 card_min{};
        ImVec2 card_max{};
        if (docked) {
            // A dock owns its rail cell, so fill it. The old card was pinned to
            // the bottom-right at a fixed 370x164 dp even when docked: the rail
            // stayed empty while the plot was squeezed until the axis numbers sat
            // on top of the residual curve they were labelling.
            const float inset = ui_px(kConvergenceCardInset);
            card_min = ImVec2(mn.x + inset, mn.y + inset);
            card_max = ImVec2(mx.x - inset, mx.y - inset);
        } else {
            const float margin = ui_px(16.0f);
            const float width = std::min(ui_px(370.0f), mx.x - mn.x - 2.0f * margin);
            const float height = std::min(ui_px(196.0f), mx.y - mn.y - 2.0f * margin);
            card_max = ImVec2(mx.x - margin, mx.y - margin);
            card_min = ImVec2(card_max.x - width, card_max.y - height);
        }
        if (card_max.x - card_min.x < ui_px(190.0f) ||
            card_max.y - card_min.y < ui_px(kConvergenceCardFloor)) {
            return;
        }

        ImFont* body = ImGui::GetFont();
        ImFont* numbers = mono != nullptr ? mono : body;
        const float body_size = body->FontSize;
        const float number_size = numbers->FontSize;
        const float pad = ui_px(13.0f);
        const float inner_x0 = card_min.x + pad;
        const float inner_x1 = card_max.x - pad;

        // The whole layout is decided before a single pixel lands. A glass card
        // is a promise that there is an instrument inside it, so it is painted
        // only once a data band is known to survive the header.
        const float axis_strip = body_size + ui_px(6.0f);
        const float caption_strip = std::max(body_size, number_size) + ui_px(7.0f);
        const float split_gap = ui_px(10.0f);
        const float title_y = card_min.y + ui_px(9.0f);
        const float readout_y = title_y + body_size + ui_px(5.0f);
        const float plots_top = readout_y + number_size + ui_px(10.0f);
        const float plots_bottom = card_max.y - pad;
        const float plots_height = plots_bottom - plots_top;
        if (plots_height < ui_px(kPlotDataFloor)) {
            return;
        }
        // A single sample is a dot, not a curve, and its number is already in the
        // header — so it gets no band of its own. The exception is a run that has
        // nothing else: then the dot is the honest instrument and it takes the
        // cell, rather than the card standing empty.
        const bool residual_curve = residual_count >= 2;
        const bool eta_curve = has_eta && pass_history_count >= 2;
        bool band_residual = residual_curve;
        bool band_eta = eta_curve;
        if (!band_residual && !band_eta) {
            band_residual = has_residual;
            band_eta = has_eta && !band_residual;
        }
        const bool split =
            band_residual && band_eta &&
            plots_height >=
                2.0f * (caption_strip + axis_strip + ui_px(kPlotDataFloor)) + split_gap;
        // Below the split threshold the residual trace carries the solve, so it
        // keeps the whole cell and the eta history falls back to its header line.
        const bool plot_eta = band_eta && (split || !band_residual);

        glass_background(dl, card_min, card_max, 9.0f, convergence_alpha);

        // Header strip: the instrument's name plus the newest real readout, so no
        // number has to be parked on top of the data it describes.
        dl->AddText(body, body_size, ImVec2(inner_x0, title_y),
                    color(palette.accent2, convergence_alpha), "solve convergence");
        char header_right[64]{};
        bool header_is_target = false;
        if (plot_eta && setup->eta_target > 0.0) {
            header_is_target = true;
            std::snprintf(header_right, sizeof(header_right), "target %.4g",
                          setup->eta_target);
        } else if (has_eta) {
            // The eta history has no band of its own in this layout; its newest
            // real number still belongs on screen.
            const auto& latest = pass_history[pass_history_count - 1];
            std::snprintf(header_right, sizeof(header_right), "pass %d   eta %.4g",
                          latest.pass + 1, latest.global_eta);
        }
        if (header_right[0] != '\0') {
            const float width =
                numbers->CalcTextSizeA(number_size, kUnbounded, 0.0f, header_right).x;
            dl->AddText(numbers, number_size,
                        ImVec2(std::max(inner_x0, inner_x1 - width), title_y),
                        color(header_is_target ? palette.status_warn : palette.text_dim,
                              convergence_alpha),
                        header_right);
        }

        char readout[160]{};
        if (has_residual) {
            const auto& latest = residual_at(residual_count - 1);
            std::snprintf(readout, sizeof(readout), "CG %d   residual %.3e", latest.iteration,
                          latest.residual);
        } else {
            const auto& latest = pass_history[pass_history_count - 1];
            std::snprintf(readout, sizeof(readout), "pass %d   eta %.4g", latest.pass + 1,
                          latest.global_eta);
        }
        dl->AddText(numbers, number_size, ImVec2(inner_x0, readout_y),
                    color(palette.text, convergence_alpha), readout);

        if (split) {
            // 58 / 42: the residual trace is sampled every few CG iterations, the
            // pass history is at most `adapt_passes + 1` points.
            const float boundary = plots_top + (plots_height - split_gap) * 0.58f;
            const auto& latest = pass_history[pass_history_count - 1];
            char eta_readout[96]{};
            std::snprintf(eta_readout, sizeof(eta_readout), "pass %d   eta %.4g",
                          latest.pass + 1, latest.global_eta);
            draw_residual_plot(dl, ImVec2(inner_x0, plots_top), ImVec2(inner_x1, boundary),
                               body, numbers, "CG residual", nullptr);
            draw_eta_plot(dl, ImVec2(inner_x0, boundary + split_gap),
                          ImVec2(inner_x1, plots_bottom), body, numbers, "adaptive eta",
                          eta_readout);
            return;
        }
        if (band_residual) {
            draw_residual_plot(dl, ImVec2(inner_x0, plots_top), ImVec2(inner_x1, plots_bottom),
                               body, numbers, nullptr, nullptr);
            return;
        }
        draw_eta_plot(dl, ImVec2(inner_x0, plots_top), ImVec2(inner_x1, plots_bottom), body,
                      numbers, nullptr, nullptr);
    }
};

LiveView::LiveView() : impl_(new Impl) {
    impl_->caption_storage.reserve(160);
    impl_->candidate_frames.reserve(128);
    impl_->chosen_heads.reserve(128);
}

LiveView::~LiveView() {
    if (impl_->attached_job != nullptr) {
        if (working(impl_->attached_job->state())) {
            assert(false && "LiveView must be detached after the worker joins");
            // Keep callback storage alive in release builds rather than leave the
            // worker with a dangling capture after a caller violates the contract.
            return;
        }
        impl_->attached_job->on_mesh_stage = {};
        impl_->attached_job->on_solve_stage = {};
        impl_->attached_job->on_pass = {};
    }
    delete impl_;
}

void LiveView::set_setup(const pipeline::SimSetup& setup) {
    assert(impl_->attached_job == nullptr && "set_setup must be called before attach");
    if (impl_->attached_job != nullptr) {
        return;
    }
    impl_->setup = setup;
}

void LiveView::set_explanation(std::optional<advisor::AdvisorExplanation> explanation,
                               std::optional<advisor::NetworkLayout> layout) {
    assert(impl_->attached_job == nullptr && "set_explanation must be called before attach");
    if (impl_->attached_job != nullptr) {
        return;
    }
    impl_->explanation = std::move(explanation);
    impl_->layout = std::move(layout);
    impl_->prepare_advisor();
}

void LiveView::attach(pipeline::SolveJob& job) {
    assert(impl_->attached_job == nullptr && "LiveView is already attached");
    assert(job.state() == pipeline::SolveJob::State::kIdle &&
           "attach is UI-thread-only while the job is idle");
    if (impl_->attached_job != nullptr || job.state() != pipeline::SolveJob::State::kIdle) {
        return;
    }
    impl_->clear_runtime();
    impl_->run_active = true;
    impl_->attached_job = &job;
    job.on_mesh_stage = [state = impl_](const pipeline::MeshStage& stage) {
        state->enqueue_mesh(stage);
    };
    job.on_solve_stage = [state = impl_](const pipeline::SolveStage& stage) {
        state->enqueue_solve(stage);
    };
    job.on_pass = [state = impl_](const pipeline::PassTrace& trace) {
        state->enqueue_pass(trace);
    };
}

void LiveView::detach(pipeline::SolveJob& job) {
    assert(impl_->attached_job == &job && "detaching a job that LiveView does not own");
    assert(!working(job.state()) && "detach is UI-thread-only after the worker joins");
    if (impl_->attached_job != &job || working(job.state())) {
        return;
    }
    job.on_mesh_stage = {};
    job.on_solve_stage = {};
    job.on_pass = {};
    impl_->progress = job.progress();
    impl_->progress_valid = true;
    impl_->worker_finished = true;
    impl_->attached_job = nullptr;
}

bool LiveView::tick(float dt, Viewport& viewport) {
    if (!impl_->run_active) {
        return false;
    }
    dt = std::isfinite(dt) ? std::max(dt, 0.0f) : 0.0f;

    pipeline::SolveJob::State job_state = pipeline::SolveJob::State::kIdle;
    if (impl_->attached_job != nullptr) {
        job_state = impl_->attached_job->state();
        impl_->progress = impl_->attached_job->progress();
        impl_->progress_valid = true;
        if (terminal(job_state)) {
            impl_->worker_finished = true;
        }
    }
    impl_->mesh_building = !impl_->worker_finished &&
                           (job_state == pipeline::SolveJob::State::kMeshing ||
                            (impl_->progress_valid && impl_->progress.phase == "mesh"));
    impl_->solving = !impl_->worker_finished &&
                     (job_state == pipeline::SolveJob::State::kSolving ||
                      (impl_->progress_valid && (impl_->progress.phase == "assemble" ||
                                                 impl_->progress.phase == "solve" ||
                                                 impl_->progress.phase == "recover")));
    if (impl_->solving) {
        impl_->add_residual(impl_->progress);
    }

    auto& meshes = impl_->drained_meshes;
    auto& solves = impl_->drained_solves;
    auto& passes = impl_->drained_passes;
    meshes.clear();
    solves.clear();
    passes.clear();
    std::size_t dropped = 0;
    {
        const std::lock_guard<std::mutex> lock(impl_->queue_mutex);
        meshes.swap(impl_->pending_meshes);
        solves.swap(impl_->pending_solves);
        passes.swap(impl_->pending_passes);
        dropped = impl_->dropped_meshes;
    }
    const bool drained_events = !meshes.empty() || !solves.empty() || !passes.empty();
    impl_->coalesced_meshes = dropped + impl_->skipped_meshes;
    for (const auto& pass : passes) {
        impl_->add_pass(pass);
    }
    for (const auto& solve : solves) {
        impl_->add_pass(solve.trace);
        impl_->result_landed = true;
    }

    if (!meshes.empty()) {
        if (meshes.size() > 1) {
            impl_->skipped_meshes += meshes.size() - 1;
            impl_->coalesced_meshes = dropped + impl_->skipped_meshes;
        }
        auto latest = std::move(meshes.back());
        std::optional<Clock::time_point> interval_start = impl_->last_stage_arrival;
        if (meshes.size() > 1) {
            interval_start = meshes[meshes.size() - 2].arrival;
        }
        if (interval_start) {
            const float interval =
                std::chrono::duration<float>(latest.arrival - *interval_start).count();
            if (interval > 0.0f && std::isfinite(interval)) {
                impl_->reveal_duration = std::clamp(interval * 0.78f, 0.16f, 1.20f);
                if (impl_->advisor_ready && impl_->explanation) {
                    const float expected_window =
                        interval * static_cast<float>(pipeline::kMeshStageNames.size());
                    impl_->advisor_frame_period =
                        std::clamp(expected_window /
                                       static_cast<float>(impl_->explanation->frames.size()),
                                   0.025f, 0.14f);
                }
            }
        }
        impl_->transition =
            impl_->current_stage && impl_->current_stage->pass != latest.stage.pass;
        if (impl_->transition) {
            viewport.set_cinema_mesh_transition(impl_->current_stage->mesh, latest.stage.mesh);
        } else {
            viewport.set_cinema_mesh(latest.stage.mesh);
        }
        impl_->current_stage = std::move(latest.stage);
        impl_->last_stage_arrival = latest.arrival;
        impl_->reveal = 0.0f;
    }

    if (impl_->current_stage) {
        const float duration = impl_->worker_finished ? std::min(impl_->reveal_duration, 0.30f)
                                                      : impl_->reveal_duration;
        impl_->reveal = std::min(1.0f, impl_->reveal + dt / std::max(duration, 0.001f));
        const float front = smoothstep(impl_->reveal);
        Viewport::CinemaView view;
        view.skeleton_alpha = 0.0f;
        view.model_alpha = 0.0f;
        view.reveal = front;
        view.shrink =
            kRevealShrink * (1.0f - smoothstep(impl_->reveal / kRevealShrinkFraction));
        view.mesh_alpha = 1.0f;
        view.arrival_band = kArrivalBand * (1.0f - smoothstep((front - 0.88f) / 0.12f));
        view.edges = true;
        // Edges explain arriving cell topology, but a fixed alpha turns a
        // 26k-cell boundary into television static and keeps obscuring the
        // body after the reveal lands. Scale by real element density and fade
        // almost all of the ink once the arrival front settles.
        const float elements = static_cast<float>(
            std::max<std::size_t>(impl_->current_stage->mesh.elements.size(), 1));
        const float density = std::clamp(std::sqrt(6000.0f / elements), 0.18f, 1.0f);
        const float settling = 1.0f - smoothstep((front - 0.72f) / 0.28f);
        view.edge_alpha = kMeshEdgeAlpha * density * (0.08f + 0.92f * settling);
        view.edge_width = 0.75f + 0.25f * density;
        view.incremental_transition = impl_->transition;
        view.transition_progress = front;
        viewport.set_cinema_view(view);
    }

    const bool advisor_visible =
        impl_->advisor_ready && impl_->mesh_building && !impl_->result_landed;
    if (advisor_visible && impl_->explanation) {
        impl_->advisor_cursor += dt / std::max(impl_->advisor_frame_period, 0.001f);
        const std::size_t last = impl_->explanation->frames.size() - 1;
        impl_->advisor_frame = std::min(static_cast<std::size_t>(impl_->advisor_cursor), last);
    }
    impl_->advisor_alpha = move_toward(impl_->advisor_alpha, advisor_visible ? 1.0f : 0.0f,
                                       dt / (advisor_visible ? 0.24f : 0.30f));

    const bool stage_visible =
        impl_->current_stage && (impl_->mesh_building || impl_->reveal < 1.0f);
    impl_->stage_alpha = move_toward(impl_->stage_alpha, stage_visible ? 1.0f : 0.0f,
                                     dt / (stage_visible ? 0.20f : 0.42f));
    const bool adaptive_eta =
        impl_->setup && impl_->setup->adapt_passes > 0 && impl_->pass_history_count > 0;
    const bool convergence_visible =
        impl_->solving && (impl_->residual_count > 0 || adaptive_eta);
    impl_->convergence_alpha =
        move_toward(impl_->convergence_alpha, convergence_visible ? 1.0f : 0.0f,
                    dt / (convergence_visible ? 0.20f : 0.42f));

    impl_->owns_canvas =
        impl_->current_stage && (impl_->mesh_building || impl_->reveal < 1.0f);
    if (impl_->worker_finished) {
        const bool reveal_finished = !impl_->current_stage || impl_->reveal >= 1.0f;
        if (reveal_finished && impl_->stage_alpha <= 0.001f &&
            impl_->advisor_alpha <= 0.001f && impl_->convergence_alpha <= 0.001f &&
            !drained_events) {
            impl_->run_active = false;
            impl_->owns_canvas = false;
            impl_->caption_storage.clear();
            meshes.clear();
            solves.clear();
            passes.clear();
            return false;
        }
    }

    impl_->update_caption();
    const bool owns_canvas = impl_->owns_canvas;
    meshes.clear();
    solves.clear();
    passes.clear();
    return owns_canvas;
}

void LiveView::draw_overlays(ImDrawList* draw_list, ImVec2 minimum, ImVec2 maximum,
                             ImFont* mono) {
    // Kept as the narrow-window extension point and for API compatibility with
    // the interactive shell. The current product layout docks every instrument
    // in the rails so the 3D model is never obscured.
    (void)draw_list;
    (void)minimum;
    (void)maximum;
    (void)mono;
}

bool LiveView::active() const { return impl_->run_active; }

bool LiveView::has_advisor_content() const {
    return impl_->advisor_ready && impl_->explanation && impl_->layout;
}

void LiveView::draw_advisor_dock(ImDrawList* draw_list, ImVec2 minimum, ImVec2 maximum,
                                 ImFont* mono) {
    if (!has_advisor_content() || draw_list == nullptr || maximum.x <= minimum.x ||
        maximum.y <= minimum.y) {
        return;
    }
    // A dock is stable product chrome, not a transient overlay. It follows the
    // live frame while work is running and retains the final real forward pass
    // afterwards so the otherwise-empty results rail remains informative.
    const float prior_alpha = impl_->advisor_alpha;
    const std::size_t prior_frame = impl_->advisor_frame;
    impl_->advisor_alpha = 1.0f;
    if (!impl_->run_active && impl_->explanation && !impl_->explanation->frames.empty()) {
        impl_->advisor_frame = impl_->explanation->frames.size() - 1;
    }
    impl_->draw_advisor(draw_list, minimum, maximum, mono, true);
    impl_->advisor_alpha = prior_alpha;
    impl_->advisor_frame = prior_frame;
}

bool LiveView::has_convergence_content() const {
    // Exactly what draw_convergence is able to plot, so the analysis rail never
    // reserves a cell for an instrument that then declines to draw itself.
    return impl_->residual_count > 0 ||
           (impl_->setup && impl_->setup->adapt_passes > 0 && impl_->pass_history_count > 0);
}

void LiveView::draw_convergence_dock(ImDrawList* draw_list, ImVec2 minimum, ImVec2 maximum,
                                     ImFont* mono) {
    if (!has_convergence_content() || draw_list == nullptr || maximum.x <= minimum.x ||
        maximum.y <= minimum.y) {
        return;
    }
    const float prior_alpha = impl_->convergence_alpha;
    impl_->convergence_alpha = 1.0f;
    impl_->draw_convergence(draw_list, minimum, maximum, mono, true);
    impl_->convergence_alpha = prior_alpha;
}

float LiveView::advisor_dock_floor() const {
    // draw_advisor insets the cell by its card margin on both edges before
    // measuring the four lanes and the candidate strip.
    return ui_px(kAdvisorLanesFloor + 2.0f * kAdvisorCardMargin);
}

float LiveView::convergence_dock_floor() const {
    // draw_convergence insets the cell, then needs its two header rows plus one
    // data band inside the card.
    return ui_px(kConvergenceCardFloor + 2.0f * kConvergenceCardInset);
}

const char* LiveView::caption() const { return impl_->caption_storage.c_str(); }

void LiveView::reset() {
    if (impl_->attached_job != nullptr) {
        assert(!working(impl_->attached_job->state()) &&
               "reset must not race a running worker");
        if (working(impl_->attached_job->state())) {
            return;
        }
        impl_->attached_job->on_mesh_stage = {};
        impl_->attached_job->on_solve_stage = {};
        impl_->attached_job->on_pass = {};
        impl_->attached_job = nullptr;
    }
    impl_->clear_all();
}

} // namespace polymesh::gui
