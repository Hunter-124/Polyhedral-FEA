// SPDX-License-Identifier: BSD-3-Clause
#include "jobs.hpp"

#include "encode.hpp"

#include "fea/traction.hpp"
#include "fea/vtu.hpp"
#include "geom/step.hpp"
#include "pipeline/scene.hpp"

#ifdef POLYMESH_WITH_ADVISOR
#include "advisor/advisor.hpp"
#endif

#include <Eigen/Eigenvalues>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace polymesh::webd {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxParts = 32;
constexpr std::size_t kMaxFinishedJobs = 32;
constexpr auto kProgressInterval = std::chrono::milliseconds(150);
constexpr auto kKeepaliveInterval = std::chrono::seconds(15);

struct TempPath {
    std::filesystem::path path;
    ~TempPath() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

struct Box {
    Eigen::Vector3d lo;
    Eigen::Vector3d hi;
};

struct ForceBox {
    Box box;
    Eigen::Vector3d force;
};

struct PartRecord {
    std::string id;
    std::string name;
    std::string kind;
    std::shared_ptr<TempPath> source;
    std::shared_ptr<pipeline::Model> model;
    json envelope;
};

struct Event {
    std::string name;
    std::string data;
};

struct JobRecord {
    std::string id;
    std::string kind;
    std::string part_id;
    std::string mesher;
    double h = 0.0;
    std::shared_ptr<PartRecord> part;
    pipeline::SolveJob worker;
    Clock::time_point started = Clock::now();
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::vector<Event> events;
    bool terminal = false;
    std::optional<pipeline::SolveResult> result;
    std::shared_ptr<TempPath> vtu;

    void append(std::string name, const json& data) {
        {
            std::lock_guard lock(mutex);
            events.push_back({std::move(name), data.dump()});
        }
        changed.notify_all();
    }

    ~JobRecord() = default;
};

std::string id_with_prefix(std::string_view prefix) {
    static std::atomic<std::uint64_t> sequence{1};
    static const std::uint64_t salt = [] {
        std::random_device source;
        return (static_cast<std::uint64_t>(source()) << 32U) ^ source();
    }();
    const std::uint64_t value = sequence.fetch_add(1, std::memory_order_relaxed) ^ salt;
    std::ostringstream text;
    text << prefix << std::hex << std::setfill('0') << std::setw(16) << value;
    return text.str();
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::filesystem::path temp_file(std::string_view prefix, std::string_view extension) {
    const auto name = id_with_prefix(prefix) + std::string(extension);
    return std::filesystem::temp_directory_path() / name;
}

HttpResponse json_response(int status, const json& body) {
    HttpResponse response;
    response.status = status;
    response.body = body.dump();
    return response;
}

std::string path_only(std::string_view target) {
    return std::string(target.substr(0, target.find('?')));
}

bool finite(double value) { return std::isfinite(value); }

Eigen::Vector3d vector3(const json& value, std::string_view field) {
    if (!value.is_array() || value.size() != 3) {
        throw std::runtime_error(std::string(field) + " must contain three numbers");
    }
    Eigen::Vector3d result;
    for (std::size_t index = 0; index < 3; ++index) {
        if (!value[index].is_number()) {
            throw std::runtime_error(std::string(field) + " must contain only numbers");
        }
        result[static_cast<Eigen::Index>(index)] = value[index].get<double>();
        if (!finite(result[static_cast<Eigen::Index>(index)])) {
            throw std::runtime_error(std::string(field) + " must be finite");
        }
    }
    return result;
}

Box box6(const json& value, std::string_view field) {
    if (!value.is_array() || value.size() != 6) {
        throw std::runtime_error(std::string(field) + " must contain six numbers");
    }
    std::array<double, 6> numbers{};
    for (std::size_t index = 0; index < numbers.size(); ++index) {
        if (!value[index].is_number()) {
            throw std::runtime_error(std::string(field) + " must contain only numbers");
        }
        numbers[index] = value[index].get<double>();
        if (!finite(numbers[index])) {
            throw std::runtime_error(std::string(field) + " must be finite");
        }
    }
    return {{std::min(numbers[0], numbers[3]), std::min(numbers[1], numbers[4]),
             std::min(numbers[2], numbers[5])},
            {std::max(numbers[0], numbers[3]), std::max(numbers[1], numbers[4]),
             std::max(numbers[2], numbers[5])}};
}

json vec_json(const Eigen::Vector3d& value) {
    return json::array({value.x(), value.y(), value.z()});
}

std::string pretty_name(std::string stem) {
    std::replace(stem.begin(), stem.end(), '_', ' ');
    if (!stem.empty()) {
        stem.front() =
            static_cast<char>(std::toupper(static_cast<unsigned char>(stem.front())));
    }
    return stem;
}

std::vector<std::pair<std::string, std::filesystem::path>>
examples_in(const std::filesystem::path& directory) {
    std::vector<std::pair<std::string, std::filesystem::path>> examples;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        return examples;
    }
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end;
         it.increment(error)) {
        if (!it->is_regular_file()) {
            continue;
        }
        const std::string extension = lower(it->path().extension().string());
        if (extension == ".step" || extension == ".stp" || extension == ".brep") {
            examples.emplace_back(it->path().stem().string(), it->path());
        }
    }
    std::sort(examples.begin(), examples.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return examples;
}

std::string constraint_defect(const fea::NodalMesh& mesh,
                              const std::vector<std::uint32_t>& fixed_nodes) {
    if (fixed_nodes.size() < 3) {
        return "fixture boxes select fewer than three boundary nodes";
    }
    Eigen::Vector3d mean = Eigen::Vector3d::Zero();
    for (const std::uint32_t node : fixed_nodes) {
        mean += mesh.nodes.at(node);
    }
    mean /= static_cast<double>(fixed_nodes.size());
    Eigen::Matrix3d covariance = Eigen::Matrix3d::Zero();
    for (const std::uint32_t node : fixed_nodes) {
        const Eigen::Vector3d delta = mesh.nodes.at(node) - mean;
        covariance += delta * delta.transpose();
    }
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(covariance);
    const Eigen::Vector3d values = solver.eigenvalues();
    if (!(values[2] > 0.0)) {
        return "fixture boundary nodes are coincident";
    }
    if (std::sqrt(std::max(0.0, values[1] / values[2])) < 1e-6) {
        return "fixture boundary nodes are collinear";
    }
    return {};
}

pipeline::BoundaryConditions boundary_conditions(const fea::NodalMesh& mesh,
                                                 const std::vector<Box>& fixtures,
                                                 const std::vector<ForceBox>& loads) {
    const auto all_faces = fea::boundary_surface_faces(mesh);
    std::set<std::uint32_t> fixed;
    for (const Box& box : fixtures) {
        const fea::LoadRegion region{box.lo, box.hi};
        const auto nodes = fea::boundary_nodes_within(mesh, all_faces, region);
        fixed.insert(nodes.begin(), nodes.end());
    }
    std::vector<std::uint32_t> fixed_nodes(fixed.begin(), fixed.end());
    if (const std::string defect = constraint_defect(mesh, fixed_nodes); !defect.empty()) {
        throw std::runtime_error(defect);
    }
    pipeline::BoundaryConditions result;
    for (const std::uint32_t node : fixed_nodes) {
        result.dirichlet.fix_node(node);
    }
    result.loads = Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    for (const ForceBox& load : loads) {
        const fea::LoadRegion region{load.box.lo, load.box.hi};
        const auto nodes = fea::boundary_nodes_within(mesh, all_faces, region);
        const auto faces = fea::faces_touching(mesh, all_faces, region);
        const double area = fea::integrated_region_area(mesh, faces, region);
        if (nodes.empty() && !(area > 0.0)) {
            throw std::runtime_error("a load box selects no boundary surface");
        }
        if (area > 0.0) {
            const auto applied = fea::consistent_region_load(mesh, faces, region, load.force);
            if (applied.conservation_error > 1e-9) {
                throw std::runtime_error("surface load assembly did not conserve its force");
            }
            result.loads += applied.loads;
        } else {
            const Eigen::Vector3d per_node = load.force / static_cast<double>(nodes.size());
            for (const std::uint32_t node : nodes) {
                result.loads.segment<3>(3 * static_cast<Eigen::Index>(node)) += per_node;
            }
        }
    }
    if (!(result.loads.norm() > 0.0)) {
        throw std::runtime_error("load boxes produce a zero resultant load vector");
    }
    return result;
}

json mesh_event_json(const pipeline::MeshStage& stage) {
    EncodedMeshSurface encoded = mesh_surface_json(stage.mesh);
    return {{"stage", stage.stage},
            {"index", stage.index},
            {"pass", stage.pass},
            {"n_elems", stage.mesh.elements.size()},
            {"n_nodes", stage.mesh.nodes.size()},
            {"emitted_elems", encoded.emitted_elements},
            {"cells", std::move(encoded.surface)}};
}

json progress_json(const pipeline::JobProgress& progress) {
    return {{"phase", progress.phase},
            {"phase_frac", progress.phase_frac},
            {"elapsed_ms",
             static_cast<std::uint64_t>(std::max(0.0, std::round(progress.elapsed_ms)))},
            {"pass", progress.pass},
            {"pass_count", progress.pass_count},
            {"cg_iter", progress.cg_iter},
            {"cg_resid", progress.cg_resid},
            {"n_elems", progress.n_elems},
            {"n_nodes", progress.n_nodes}};
}

json pass_json(const pipeline::PassTrace& trace) {
    return {{"pass", trace.pass},
            {"n_elems", trace.n_elems},
            {"n_nodes", trace.n_nodes},
            {"dof", trace.n_dof},
            {"global_eta", trace.global_eta},
            {"eta_p90", trace.eta_p90},
            {"mesh_ms", trace.mesh_ms},
            {"solve_ms", trace.solve_ms},
            {"cg_iters", trace.cg_iters},
            {"solve_method", trace.solve_method}};
}

json result_json(const pipeline::SolveResult& result) {
    return {{"n_nodes", result.volume_mesh.nodes.size()},
            {"n_elems", result.volume_mesh.elements.size()},
            {"dof", result.displacement.size()},
            {"max_von_mises", result.max_von_mises},
            {"max_displacement", result.max_displacement},
            {"global_eta", result.global_eta},
            {"mesh_note", result.mesh_note},
            {"solver_note", result.solver_note},
            {"surface", result_surface_json(result)}};
}

#ifdef POLYMESH_WITH_ADVISOR
float percentile(std::vector<float>& values) {
    if (values.empty()) {
        return 1.0F;
    }
    const auto at = values.begin() +
                    static_cast<std::ptrdiff_t>(0.98 * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), at, values.end());
    return *at > 0.0F ? *at : 1.0F;
}

const std::vector<float>& frame_layer(const advisor::ActivationFrame& frame,
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

json advisor_json(const advisor::AdvisorExplanation& explanation,
                  const advisor::NetworkLayout& layout, double diagonal) {
    if (layout.layers.size() != 4 || explanation.frames.empty()) {
        throw std::runtime_error("advisor activation layout is incomplete");
    }
    std::array<float, 4> layer_scale{};
    std::vector<float> pool;
    for (std::size_t layer = 0; layer < layer_scale.size(); ++layer) {
        pool.clear();
        for (const auto& frame : explanation.frames) {
            for (const float value : frame_layer(frame, layer)) {
                pool.push_back(std::abs(value));
            }
        }
        layer_scale[layer] = percentile(pool);
    }
    pool.clear();
    for (const auto& frame : explanation.frames) {
        for (std::size_t block_index = 0; block_index < layout.edges.size(); ++block_index) {
            const auto& block = layout.edges[block_index];
            if (block_index >= 3) {
                break;
            }
            const auto& source = frame_layer(frame, block_index);
            if (block.weights.size() != block.rows * block.cols ||
                source.size() != block.cols) {
                continue;
            }
            for (std::size_t row = 0; row < block.rows; ++row) {
                for (std::size_t column = 0; column < block.cols; ++column) {
                    pool.push_back(
                        std::abs(block.weights[row * block.cols + column] * source[column]));
                }
            }
        }
    }
    const float contribution = percentile(pool);

    json layers = json::array();
    for (std::size_t index = 0; index < layout.layers.size(); ++index) {
        static constexpr std::array<std::string_view, 4> kNames{"input", "fc1", "fc2",
                                                                "heads"};
        layers.push_back(
            {{"name", std::string(kNames[index])}, {"size", layout.layers[index].size}});
    }
    json edges = json::array();
    for (const auto& block : layout.edges) {
        auto normalize_name = [](const std::string& name) {
            if (name == "trunk.fc1") {
                return std::string("fc1");
            }
            if (name == "trunk.fc2") {
                return std::string("fc2");
            }
            return name;
        };
        edges.push_back({{"from", normalize_name(block.from)},
                         {"to", normalize_name(block.to)},
                         {"rows", block.rows},
                         {"cols", block.cols},
                         {"weights", block.weights}});
    }
    const auto& labels = layout.layers.back().labels;
    int winner = -1;
    const std::string winner_label = "policy_mesher_logit_" + explanation.decision.mesher;
    for (std::size_t index = 0; index < labels.size(); ++index) {
        if (labels[index] == winner_label) {
            winner = static_cast<int>(index);
            break;
        }
    }
    json frames = json::array();
    for (const auto& frame : explanation.frames) {
        frames.push_back({{"candidate", frame.candidate},
                          {"recommended", frame.recommended},
                          {"gate_pass", frame.gate_pass},
                          {"score", frame.score},
                          {"action",
                           {{"mesher", frame.action.mesher},
                            {"h", frame.action.h_rel * diagonal},
                            {"order", frame.action.order},
                            {"adapt", frame.action.adapt_passes}}},
                          {"input", frame.input},
                          {"fc1", frame.fc1},
                          {"fc2", frame.fc2},
                          {"heads", frame.heads}});
    }
    return {{"gate_threshold", explanation.gate_threshold},
            {"winner", winner},
            {"layers", std::move(layers)},
            {"edges", std::move(edges)},
            {"head_labels", labels},
            {"scale",
             {{"input", layer_scale[0]},
              {"fc1", layer_scale[1]},
              {"fc2", layer_scale[2]},
              {"heads", layer_scale[3]},
              {"contribution", contribution}}},
            {"frames", std::move(frames)}};
}
#endif

HttpResponse part_response(const PartRecord& part) {
    const std::string payload = part.envelope.dump();
    HttpResponse response;
    response.status = 200;
    response.body.reserve(payload.size() + 20);
    response.body = "{\"ok\":true,\"part\":";
    response.body += payload;
    response.body += '}';
    return response;
}

} // namespace

struct JobService::Impl {
    explicit Impl(ServerOptions incoming) : options(std::move(incoming)) {
#ifdef POLYMESH_WITH_ADVISOR
        if (!options.advisor_dir.empty()) {
            advisor_instance.emplace(options.advisor_dir,
                                     advisor::AdvisorObjective::kAccuracy);
        }
#else
        if (!options.advisor_dir.empty()) {
            throw std::runtime_error(
                "--advisor requires a build with POLYMESH_WITH_ADVISOR enabled");
        }
#endif
    }

    ServerOptions options;
    mutable std::mutex store_mutex;
    std::map<std::string, std::shared_ptr<PartRecord>, std::less<>> parts;
    std::deque<std::string> part_order;
    std::map<std::string, std::shared_ptr<JobRecord>, std::less<>> jobs;
    std::deque<std::string> job_order;
#ifdef POLYMESH_WITH_ADVISOR
    std::optional<advisor::Advisor> advisor_instance;
    std::mutex advisor_mutex;
#endif

    std::shared_ptr<PartRecord> load_part(const std::filesystem::path& path,
                                          std::string display_name,
                                          std::shared_ptr<TempPath> source) {
        auto model = std::make_shared<pipeline::Model>(pipeline::Model::load(path.string()));
        const Eigen::Vector3d extent = model->bbox_max - model->bbox_min;
        const double diagonal = extent.norm();
        if (!(diagonal > 0.0) || !finite(diagonal)) {
            throw std::runtime_error("part bounding box has no finite positive diagonal");
        }
        auto part = std::make_shared<PartRecord>();
        part->id = id_with_prefix("p_");
        part->name = std::move(display_name);
        part->kind = lower(path.extension().string());
        if (!part->kind.empty() && part->kind.front() == '.') {
            part->kind.erase(part->kind.begin());
        }
        if (part->kind == "stp") {
            part->kind = "step";
        }
        part->source = std::move(source);
        part->model = std::move(model);
        // The GUI presets are contextual rather than a shareable numeric rule.
        // Use the documented web heuristic: one twenty-fifth of the real bbox diagonal.
        part->envelope = {{"id", part->id},
                          {"name", part->name},
                          {"kind", part->kind},
                          {"triangles", part->model->surface.triangles.size()},
                          {"regions", part->model->region_count},
                          {"bbox_min", vec_json(part->model->bbox_min)},
                          {"bbox_max", vec_json(part->model->bbox_max)},
                          {"suggested_h", diagonal / 25.0},
                          {"surface", part_surface_json(*part->model)}};
        {
            std::lock_guard lock(store_mutex);
            while (part_order.size() >= kMaxParts) {
                parts.erase(part_order.front());
                part_order.pop_front();
            }
            parts.emplace(part->id, part);
            part_order.push_back(part->id);
        }
        return part;
    }

    ~Impl() {
        std::vector<std::shared_ptr<JobRecord>> active;
        {
            std::lock_guard lock(store_mutex);
            for (const auto& [id, job] : jobs) {
                (void)id;
                active.push_back(job);
            }
        }
        for (const auto& job : active) {
            job->worker.request_cancel();
        }
        for (const auto& job : active) {
            std::unique_lock lock(job->mutex);
            job->changed.wait(lock, [&] { return job->terminal; });
        }
    }

    std::shared_ptr<PartRecord> find_part(std::string_view id) const {
        std::lock_guard lock(store_mutex);
        const auto it = parts.find(id);
        return it == parts.end() ? nullptr : it->second;
    }

    std::shared_ptr<JobRecord> find_job(std::string_view id) const {
        std::lock_guard lock(store_mutex);
        const auto it = jobs.find(id);
        return it == jobs.end() ? nullptr : it->second;
    }

    void evict_finished_jobs() {
        std::lock_guard lock(store_mutex);
        std::size_t finished = 0;
        for (const auto& [id, job] : jobs) {
            (void)id;
            std::lock_guard job_lock(job->mutex);
            finished += job->terminal ? 1U : 0U;
        }
        while (finished > kMaxFinishedJobs && !job_order.empty()) {
            const std::string id = job_order.front();
            job_order.pop_front();
            const auto it = jobs.find(id);
            if (it == jobs.end()) {
                continue;
            }
            bool terminal = false;
            {
                std::lock_guard job_lock(it->second->mutex);
                terminal = it->second->terminal;
            }
            if (terminal) {
                jobs.erase(it);
                --finished;
            } else {
                job_order.push_back(id);
            }
        }
    }

    void monitor(const std::shared_ptr<JobRecord>& job) {
        std::string last_progress;
        while (true) {
            const json progress = progress_json(job->worker.progress());
            const std::string serialized = progress.dump();
            if (serialized != last_progress) {
                job->append("progress", progress);
                last_progress = serialized;
            }
            const auto state = job->worker.state();
            if (state == pipeline::SolveJob::State::kDone) {
                auto result = job->worker.take_result();
                if (!result) {
                    finish(job, "failed", "solve completed without a result");
                    return;
                }
                try {
                    job->append("result", result_json(*result));
                } catch (const std::exception& error) {
                    finish(job, "failed", error.what());
                    return;
                }
                if (!result->mesh_note.empty()) {
                    job->append("note", {{"text", result->mesh_note}});
                }
                if (!result->solver_note.empty()) {
                    job->append("note", {{"text", result->solver_note}});
                }
                {
                    std::lock_guard lock(job->mutex);
                    job->result.emplace(std::move(*result));
                }
                finish(job, "done", "");
                return;
            }
            if (state == pipeline::SolveJob::State::kMeshDone) {
                (void)job->worker.take_mesh();
                finish(job, "done", "");
                return;
            }
            if (state == pipeline::SolveJob::State::kCancelled) {
                finish(job, "cancelled", job->worker.status_text());
                return;
            }
            if (state == pipeline::SolveJob::State::kFailed) {
                finish(job, "failed", job->worker.status_text());
                return;
            }
            std::this_thread::sleep_for(kProgressInterval);
        }
    }

    void finish(const std::shared_ptr<JobRecord>& job, std::string_view state,
                std::string message) {
        const auto elapsed =
            std::chrono::duration<double, std::milli>(Clock::now() - job->started);
        job->append("done", {{"state", std::string(state)},
                             {"message", std::move(message)},
                             {"elapsed_ms", static_cast<std::uint64_t>(
                                                std::max(0.0, std::round(elapsed.count())))}});
        {
            std::lock_guard lock(job->mutex);
            job->terminal = true;
        }
        job->changed.notify_all();
        evict_finished_jobs();
    }

    std::shared_ptr<JobRecord> start_job(const json& body) {
        if (!body.is_object()) {
            throw std::runtime_error("job request must be a JSON object");
        }
        const std::string part_id = body.value("part", "");
        auto part = find_part(part_id);
        if (!part) {
            throw std::runtime_error("unknown part id");
        }
        const std::string kind = body.value("kind", "");
        if (kind != "solve" && kind != "mesh") {
            throw std::runtime_error("kind must be 'solve' or 'mesh'");
        }
        const double h = body.value("h", 0.0);
        const double youngs = body.value("E", 200e9);
        const double poisson = body.value("nu", 0.3);
        const std::string mesher_name = body.value("mesher", "hybrid");
        const int adapt = body.value("adapt_passes", 0);
        const double eta_target = body.value("eta_target", 0.0);
        const int skin = body.value("skin_layers", 2);
        const bool feature = body.value("feature_grading", true);
        if (h < 0.0 || !finite(h)) {
            throw std::runtime_error("h must be a finite nonnegative number");
        }
        if (!(youngs > 0.0) || !finite(youngs)) {
            throw std::runtime_error("E must be a finite positive number");
        }
        if (!(poisson > -1.0 && poisson < 0.5) || !finite(poisson)) {
            throw std::runtime_error("nu must be finite and lie in (-1, 0.5)");
        }
        if (adapt < 0 || skin < 1 || !(eta_target >= 0.0) || !finite(eta_target)) {
            throw std::runtime_error("adapt_passes, eta_target, or skin_layers is invalid");
        }
        const auto mesher = pipeline::mesher_from_name(mesher_name);
        if (!mesher) {
            throw std::runtime_error("unknown mesher name");
        }

        std::vector<Box> fixture_boxes;
        const auto fixtures_it = body.find("fixtures");
        if (fixtures_it != body.end()) {
            if (!fixtures_it->is_array()) {
                throw std::runtime_error("fixtures must be an array of boxes");
            }
            for (const auto& value : *fixtures_it) {
                fixture_boxes.push_back(box6(value, "fixture box"));
            }
        }
        std::vector<ForceBox> load_boxes;
        const auto loads_it = body.find("loads");
        if (loads_it != body.end()) {
            if (!loads_it->is_array()) {
                throw std::runtime_error("loads must be an array");
            }
            for (const auto& value : *loads_it) {
                if (!value.is_object() || !value.contains("box") || !value.contains("force")) {
                    throw std::runtime_error("each load needs box and force fields");
                }
                load_boxes.push_back(
                    {box6(value.at("box"), "load box"), vector3(value.at("force"), "force")});
            }
        }
        if (kind == "solve" && (fixture_boxes.empty() || load_boxes.empty())) {
            throw std::runtime_error(
                "solve jobs require at least one fixture and one load box");
        }

        pipeline::SimSetup setup;
        setup.youngs_modulus = youngs;
        setup.poissons_ratio = poisson;
        setup.mesh_size = h;
        setup.max_mem_gb = options.max_mem_gb;
        setup.max_elems = options.max_elems;
        setup.max_dof = options.max_dof;
        setup.use_feature_grading = feature;
        setup.spectral_smooth = true;
        setup.adapt_passes = adapt;
        setup.eta_target = eta_target;
        setup.p_elevate = true;
        setup.skin_layers = skin;
        setup.mesher = *mesher;
        if (kind == "solve") {
            setup.boundary_builder = [fixtures = fixture_boxes,
                                      loads = load_boxes](const fea::NodalMesh& mesh) {
                return boundary_conditions(mesh, fixtures, loads);
            };
        }

        auto job = std::make_shared<JobRecord>();
        job->id = id_with_prefix("j_");
        job->kind = kind;
        job->part_id = part_id;
        job->mesher = mesher_name;
        job->h = h;
        job->part = part;
        job->started = Clock::now();
        job->append("hello", {{"job", job->id},
                              {"kind", kind},
                              {"part", part_id},
                              {"mesher", mesher_name},
                              {"h", h}});

#ifdef POLYMESH_WITH_ADVISOR
        if (advisor_instance && advisor_instance->has_activations()) {
            std::vector<pipeline::RefineRegion> fix_regions;
            std::vector<pipeline::RefineRegion> load_regions;
            Eigen::Vector3d total_force = Eigen::Vector3d::Zero();
            for (const Box& box : fixture_boxes) {
                fix_regions.push_back({box.lo, box.hi, 0.5});
            }
            for (const ForceBox& load : load_boxes) {
                load_regions.push_back({load.box.lo, load.box.hi, 0.25});
                total_force += load.force;
            }
            const double force_norm = total_force.norm();
            // Materialised on both branches: Eigen's quotient expression and
            // ZeroReturnType are distinct types, so `?:` cannot unify them.
            Eigen::Vector3d direction = Eigen::Vector3d::Zero();
            if (force_norm > 0.0) {
                direction = total_force / force_norm;
            }
            const auto features = pipeline::extract_case_features(
                *part->model, fix_regions, load_regions, direction, poisson);
            std::lock_guard advisor_lock(advisor_mutex);
            const auto explanation =
                advisor_instance->explain(features, static_cast<double>(options.max_dof));
            const double diagonal = (part->model->bbox_max - part->model->bbox_min).norm();
            job->append("advisor",
                        advisor_json(explanation, advisor_instance->layout(), diagonal));
        }
#endif

        std::weak_ptr<JobRecord> weak_job = job;
        job->worker.on_mesh_stage = [weak_job](const pipeline::MeshStage& stage) {
            if (auto strong = weak_job.lock()) {
                strong->append("mesh", mesh_event_json(stage));
            }
        };
        // Pass telemetry comes from `on_solve_stage`, not `on_pass`.
        //
        // `pipeline` gates `on_pass` behind `setup.adapt_passes > 0`
        // (src/pipeline/src/scene.cpp:7900-7906), so a non-adaptive study —
        // which is the ordinary first study a user runs — emitted no `pass`
        // event at all, and the client had no solver method and no mesh/solve
        // timings to show. `on_solve_stage` fires once per pass in every case
        // and carries the *identical* `PassTrace` built at the same point, so
        // reading it here makes the telemetry uniform without inventing a
        // number. It costs one `SolveResult` copy per pass, which for a
        // streaming client is a cost we are already paying elsewhere.
        job->worker.on_solve_stage = [weak_job](const pipeline::SolveStage& stage) {
            if (auto strong = weak_job.lock()) {
                strong->append("pass", pass_json(stage.trace));
            }
        };
        if (kind == "solve") {
            job->worker.start(*part->model, setup);
        } else {
            job->worker.start_mesh(*part->model, setup);
        }
        {
            std::lock_guard lock(store_mutex);
            jobs.emplace(job->id, job);
            job_order.push_back(job->id);
        }
        std::thread([this, job] { monitor(job); }).detach();
        return job;
    }

    void stream_events(const std::shared_ptr<JobRecord>& job, HttpConnection& connection) {
        if (!connection.begin_sse()) {
            return;
        }
        std::size_t cursor = 0;
        while (true) {
            std::unique_lock lock(job->mutex);
            if (cursor >= job->events.size() && !job->terminal) {
                const bool changed = job->changed.wait_for(lock, kKeepaliveInterval, [&] {
                    return cursor < job->events.size() || job->terminal;
                });
                if (!changed) {
                    lock.unlock();
                    if (!connection.send_sse_comment("keepalive")) {
                        return;
                    }
                    continue;
                }
            }
            while (cursor < job->events.size()) {
                const Event event = job->events[cursor++];
                lock.unlock();
                if (!connection.send_sse(event.name, event.data)) {
                    return;
                }
                lock.lock();
            }
            if (job->terminal && cursor >= job->events.size()) {
                return;
            }
        }
    }

    bool handle(const HttpRequest& request, HttpConnection& connection) {
        const std::string path = path_only(request.target);
        if (path == "/api/health" && request.method == "GET") {
            connection.send_response(
                json_response(200, {{"ok", true},
                                    {"version", POLYMESH_VERSION},
                                    {"advisor", advisor_available()},
                                    {"occ", geom::occ_enabled()},
                                    {"threads", std::thread::hardware_concurrency()}}));
            return true;
        }
        if (path == "/api/examples" && request.method == "GET") {
            json examples = json::array();
            for (const auto& [id, example_path] : examples_in(options.examples_dir)) {
                examples.push_back({{"id", id},
                                    {"name", pretty_name(id)},
                                    {"path", example_path.filename().string()}});
            }
            connection.send_response(
                json_response(200, {{"ok", true}, {"examples", examples}}));
            return true;
        }
        if (path == "/api/parts" && request.method == "POST") {
            const std::string_view supplied = request.header("x-polymesh-filename");
            if (supplied.empty()) {
                connection.send_response(json_error(400, "X-PolyMesh-Filename is required"));
                return true;
            }
            const std::filesystem::path filename =
                std::filesystem::path(std::string(supplied)).filename();
            const std::string extension = lower(filename.extension().string());
            if (extension != ".step" && extension != ".stp" && extension != ".brep" &&
                extension != ".stl") {
                connection.send_response(
                    json_error(400, "part filename must end in .step, .stp, .brep, or .stl"));
                return true;
            }
            if (request.body.empty()) {
                connection.send_response(json_error(400, "uploaded part is empty"));
                return true;
            }
            auto source = std::make_shared<TempPath>();
            source->path = temp_file("polymesh_part_", extension);
            std::ofstream output(source->path, std::ios::binary);
            output.write(reinterpret_cast<const char*>(request.body.data()),
                         static_cast<std::streamsize>(request.body.size()));
            output.close();
            if (!output) {
                connection.send_response(json_error(500, "could not store uploaded part"));
                return true;
            }
            try {
                const auto part = load_part(source->path, filename.stem().string(), source);
                connection.send_response(part_response(*part));
            } catch (const std::exception& error) {
                connection.send_response(json_error(400, error.what()));
            }
            return true;
        }
        constexpr std::string_view kExamplePrefix = "/api/parts/example/";
        if (request.method == "POST" && path.starts_with(kExamplePrefix)) {
            const std::string id = path.substr(kExamplePrefix.size());
            const auto examples = examples_in(options.examples_dir);
            const auto it = std::find_if(examples.begin(), examples.end(),
                                         [&](const auto& item) { return item.first == id; });
            if (it == examples.end()) {
                connection.send_response(json_error(404, "example not found"));
                return true;
            }
            try {
                const auto part = load_part(it->second, pretty_name(id), nullptr);
                connection.send_response(part_response(*part));
            } catch (const std::exception& error) {
                connection.send_response(json_error(400, error.what()));
            }
            return true;
        }
        if (path == "/api/jobs" && request.method == "POST") {
            try {
                const json body = json::parse(request.body.begin(), request.body.end());
                const auto job = start_job(body);
                connection.send_response(json_response(202, {{"ok", true}, {"job", job->id}}));
            } catch (const json::exception& error) {
                connection.send_response(
                    json_error(400, std::string("invalid JSON: ") + error.what()));
            } catch (const std::exception& error) {
                connection.send_response(json_error(400, error.what()));
            }
            return true;
        }
        constexpr std::string_view kJobPrefix = "/api/jobs/";
        if (path.starts_with(kJobPrefix)) {
            const std::string suffix = path.substr(kJobPrefix.size());
            const auto slash = suffix.find('/');
            const std::string id = suffix.substr(0, slash);
            auto job = find_job(id);
            if (!job) {
                connection.send_response(json_error(404, "job not found"));
                return true;
            }
            const std::string resource =
                slash == std::string::npos ? std::string{} : suffix.substr(slash + 1);
            if (resource == "events" && request.method == "GET") {
                if (request.head) {
                    connection.begin_sse();
                } else {
                    stream_events(job, connection);
                }
                return true;
            }
            if (resource.empty() && request.method == "DELETE") {
                job->worker.request_cancel();
                connection.send_response(
                    json_response(200, {{"ok", true}, {"cancelled", true}}));
                return true;
            }
            if (resource == "result.vtu" && request.method == "GET") {
                std::filesystem::path vtu_path;
                {
                    std::lock_guard lock(job->mutex);
                    if (!job->result) {
                        connection.send_response(json_error(404, "result is not ready"));
                        return true;
                    }
                    if (!job->vtu) {
                        auto temp = std::make_shared<TempPath>();
                        temp->path = temp_file("polymesh_result_", ".vtu");
                        fea::VtuPointData displacement{
                            "displacement", {}, job->result->displacement};
                        fea::VtuPointData stress{"von_mises", job->result->von_mises, {}};
                        fea::VtuPointData magnitude{
                            "u_magnitude", job->result->u_magnitude, {}};
                        fea::VtuPointData nodal_eta{"nodal_eta", job->result->nodal_eta, {}};
                        fea::VtuCellData element_eta{"element_eta", job->result->element_eta};
                        fea::write_vtu(temp->path, job->result->volume_mesh,
                                       {displacement, stress, magnitude, nodal_eta},
                                       {element_eta});
                        job->vtu = std::move(temp);
                    }
                    vtu_path = job->vtu->path;
                }
                connection.send_file(
                    200, "application/octet-stream", vtu_path.string(),
                    {{"Content-Disposition", "attachment; filename=polymesh-result.vtu"}});
                return true;
            }
            connection.send_response(json_error(404, "job resource not found"));
            return true;
        }
        if (path.starts_with("/api/")) {
            connection.send_response(json_error(404, "API endpoint not found"));
            return true;
        }
        return false;
    }

    bool advisor_available() const {
#ifdef POLYMESH_WITH_ADVISOR
        return advisor_instance.has_value();
#else
        return false;
#endif
    }
};

JobService::JobService(ServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
JobService::~JobService() = default;

bool JobService::advisor_available() const { return impl_->advisor_available(); }

bool JobService::handle(const HttpRequest& request, HttpConnection& connection) {
    return impl_->handle(request, connection);
}

} // namespace polymesh::webd
