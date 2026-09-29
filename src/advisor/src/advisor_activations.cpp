// SPDX-License-Identifier: BSD-3-Clause
// Network drawing: activation_layout.json validation and the tap-carrying
// entry points (`explain`, `taps`).
#include "advisor_internal.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace polymesh::advisor {

using detail::json;
using detail::kActivationOutputNames;
using detail::kOutputNames;
using detail::read_json;

namespace {

/// Sidecar schema tag. Refusing an unknown tag is the point: the layout decides
/// which neuron a drawn edge connects, and a future revision that reorders
/// `layers` or transposes `weights` would still parse.
constexpr const char* kActivationLayoutSchema = "polymesh.advisor.activation_layout/1";

/// The `layers` names, in the order `AdvisorNet.activations()` emits them.
constexpr std::array<const char*, 4> kLayerNames{"input", "trunk.fc1", "trunk.fc2", "heads"};

} // namespace

// Static picture of the deployed network (activation_layout.json), written by
// scripts/advisor/export_onnx.py beside the graph it describes.
//
// Optional, unlike every other artifact this file reads: it only enables
// DRAWING the network, so a model directory without it must still load and
// recommend bit-for-bit the same. It is not reconstructed from the graph's own
// shapes: which weight row belongs to which head, and which input column feeds
// which trunk neuron after the embedding concatenation, are not recoverable
// from them, and a layout inferred that far would draw edges between the wrong
// neurons. Everything here is validated against the graph, and any single
// disagreement drops the whole facility rather than half of it.
void Advisor::Impl::load_activation_layout(const std::filesystem::path& dir,
                                           bool graph_has_taps) {
    layout = NetworkLayout{};
    activations_available = false;
    const std::string remedy =
        ". Activations are unavailable; the advisor recommends normally without them. "
        "Re-export "
        "with python scripts/advisor/export_onnx.py to draw the network.";
    const std::filesystem::path path = dir / "activation_layout.json";
    if (!graph_has_taps) {
        activation_note = "advisor: model.onnx exports no trunk activation taps" + remedy;
        return;
    }
    if (!std::filesystem::exists(path)) {
        activation_note = "advisor: no activation_layout.json in " + dir.string() + remedy;
        return;
    }

    // Validation failures are thrown and caught here. They are genuine errors
    // about the sidecar, so they are raised where they are detected with the
    // detail attached, and converted to "no activations" at exactly one place.
    NetworkLayout parsed;
    try {
        const json doc = read_json(path);
        const std::string schema = doc.value("schema", std::string{});
        if (schema != kActivationLayoutSchema) {
            // A future revision is free to reorder `layers` or transpose
            // `weights` and would still parse cleanly here, so the tag is
            // checked rather than the shape alone.
            throw AdvisorError("advisor: activation_layout.json schema is '" + schema +
                               "', expected '" + kActivationLayoutSchema + "'");
        }
        const auto taps = doc.value("activation_outputs", std::vector<std::string>{});
        if (taps.size() != kActivationOutputNames.size()) {
            throw AdvisorError("advisor: activation_layout.json activation_outputs has " +
                               std::to_string(taps.size()) + " entries, expected " +
                               std::to_string(kActivationOutputNames.size()));
        }
        for (std::size_t i = 0; i < taps.size(); ++i) {
            if (taps[i] != kActivationOutputNames[i]) {
                throw AdvisorError("advisor: activation_layout.json activation_outputs[" +
                                   std::to_string(i) + "] is '" + taps[i] + "', expected '" +
                                   kActivationOutputNames[i] + "'");
            }
        }
        if (!doc.contains("hidden") || !doc.at("hidden").is_number_unsigned()) {
            throw AdvisorError("advisor: activation_layout.json has no unsigned 'hidden'");
        }
        const auto hidden = doc.at("hidden").get<std::size_t>();

        if (!doc.contains("layers") || !doc.at("layers").is_array() ||
            doc.at("layers").size() != kLayerNames.size()) {
            throw AdvisorError("advisor: activation_layout.json needs exactly " +
                               std::to_string(kLayerNames.size()) + " layers");
        }
        for (std::size_t i = 0; i < kLayerNames.size(); ++i) {
            const json& entry = doc.at("layers")[i];
            NetworkLayer layer;
            layer.name = entry.value("name", std::string{});
            if (layer.name != kLayerNames[i]) {
                throw AdvisorError("advisor: activation_layout.json layer " +
                                   std::to_string(i) + " is '" + layer.name + "', expected '" +
                                   kLayerNames[i] + "'");
            }
            if (!entry.contains("size") || !entry.at("size").is_number_unsigned() ||
                entry.at("size").get<std::size_t>() == 0) {
                throw AdvisorError("advisor: activation_layout.json layer '" + layer.name +
                                   "' has no positive 'size'");
            }
            layer.size = entry.at("size").get<std::size_t>();
            if (entry.contains("labels")) {
                layer.labels = entry.at("labels").get<std::vector<std::string>>();
                if (!layer.labels.empty() && layer.labels.size() != layer.size) {
                    throw AdvisorError("advisor: activation_layout.json layer '" + layer.name +
                                       "' has " + std::to_string(layer.labels.size()) +
                                       " labels for " + std::to_string(layer.size) + " units");
                }
            }
            parsed.layers.push_back(std::move(layer));
        }

        // The hidden layers are the trunk, both `hidden` wide by construction
        // (model.py:fc1/fc2). Checking both against the declared width catches a
        // sidecar copied from a model trained at a different capacity.
        if (parsed.layers[1].size != hidden || parsed.layers[2].size != hidden) {
            throw AdvisorError("advisor: activation_layout.json trunk layers are " +
                               std::to_string(parsed.layers[1].size) + "/" +
                               std::to_string(parsed.layers[2].size) +
                               " wide, not the declared "
                               "hidden width " +
                               std::to_string(hidden));
        }

        // Ten regressors plus the failure logit plus one policy dimension per
        // action dim -- the same accounting `evaluate()` unpacks, so a drawn
        // "heads" layer cannot have more or fewer circles than the graph has
        // outputs.
        const std::size_t expected_heads = kOutputNames.size() - 1 + action_dims.size();
        if (parsed.layers[3].size != expected_heads) {
            throw AdvisorError("advisor: activation_layout.json 'heads' has " +
                               std::to_string(parsed.layers[3].size) + " units, expected " +
                               std::to_string(expected_heads) + " for " +
                               std::to_string(kOutputNames.size() - 1) +
                               " scalar heads plus " + std::to_string(action_dims.size()) +
                               " policy dims");
        }
        // Head ORDER matters more than head count: the drawing labels the lit
        // circle that produced the recommendation, and a permuted heads layer
        // would attribute the decision to the wrong output.
        if (!parsed.layers[3].labels.empty()) {
            for (std::size_t i = 0; i + 1 < kOutputNames.size(); ++i) {
                if (parsed.layers[3].labels[i] != kOutputNames[i]) {
                    throw AdvisorError("advisor: activation_layout.json 'heads' label " +
                                       std::to_string(i) + " is '" +
                                       parsed.layers[3].labels[i] + "', expected '" +
                                       kOutputNames[i] + "'");
                }
            }
        }

        // Tie the input layer to the graph's own input width. The trunk input is
        // not the model input: model.py:trunk_input drops the two categorical
        // columns and appends one embedding block for each, so the width is
        // `input_columns - 2 + 2 * emb_dim`. `emb_dim` is not shipped to C++, so
        // what is checked is the part that does not depend on it: every named
        // input-layer label must be a real input column, exactly the two
        // categorical columns may be missing, and the unnamed remainder (the two
        // embedding blocks) must split evenly.
        const NetworkLayer& input_layer = parsed.layers[0];
        if (input_layer.labels.size() != input_layer.size) {
            throw AdvisorError(
                "advisor: activation_layout.json 'input' layer must label all " +
                std::to_string(input_layer.size) +
                " units; the labels are what attaches an edge to a column");
        }
        std::size_t named = 0;
        for (const std::string& label : input_layer.labels) {
            if (std::find(input_columns.begin(), input_columns.end(), label) !=
                input_columns.end()) {
                ++named;
            }
        }
        if (input_columns.size() < 2 || named != input_columns.size() - 2) {
            throw AdvisorError("advisor: activation_layout.json 'input' layer names " +
                               std::to_string(named) + " of the graph's " +
                               std::to_string(input_columns.size()) +
                               " input columns, expected all but the two categorical ones");
        }
        if ((input_layer.size - named) % 2 != 0) {
            throw AdvisorError("advisor: activation_layout.json 'input' layer has " +
                               std::to_string(input_layer.size - named) +
                               " embedding units, which is not two equal blocks");
        }

        // Widths as the GRAPH declares them, where it declares them statically.
        // This is the authoritative check that `trunk_input` really is
        // `layers[0]` wide: everything above is arithmetic on the sidecar's own
        // numbers, this compares them against the tensors that will actually
        // arrive. A dynamic dimension is left unchecked rather than assumed --
        // `forward()` re-checks every tap width per pass anyway.
        const std::array<std::size_t, 3> tap_expect{parsed.layers[0].size, hidden, hidden};
        for (std::size_t i = 0; i < kActivationOutputNames.size(); ++i) {
            const auto shape = session->GetOutputTypeInfo(kOutputNames.size() + i)
                                   .GetTensorTypeAndShapeInfo()
                                   .GetShape();
            if (shape.size() != 2) {
                throw AdvisorError("advisor: model.onnx tap '" +
                                   std::string(kActivationOutputNames[i]) + "' is rank " +
                                   std::to_string(shape.size()) + ", expected [batch, width]");
            }
            if (shape[1] > 0 && static_cast<std::size_t>(shape[1]) != tap_expect[i]) {
                throw AdvisorError("advisor: model.onnx tap '" +
                                   std::string(kActivationOutputNames[i]) + "' is " +
                                   std::to_string(shape[1]) +
                                   " wide, but activation_layout.json declares " +
                                   std::to_string(tap_expect[i]));
            }
        }

        // Weight blocks. `rows`/`cols` are validated against the layers they
        // join AND against the nesting of `weights`, because those are two
        // independent claims: the header pair is what a consumer indexes with,
        // the nesting is what the numbers actually are, and a mismatch between
        // them would transpose the drawn network.
        if (!doc.contains("edges") || !doc.at("edges").is_array() ||
            doc.at("edges").size() + 1 != kLayerNames.size()) {
            throw AdvisorError("advisor: activation_layout.json needs exactly " +
                               std::to_string(kLayerNames.size() - 1) + " edge blocks");
        }
        for (std::size_t i = 0; i + 1 < kLayerNames.size(); ++i) {
            const json& entry = doc.at("edges")[i];
            NetworkEdges edges;
            edges.from = entry.value("from", std::string{});
            edges.to = entry.value("to", std::string{});
            if (edges.from != kLayerNames[i] || edges.to != kLayerNames[i + 1]) {
                throw AdvisorError("advisor: activation_layout.json edge " +
                                   std::to_string(i) + " joins '" + edges.from + "' to '" +
                                   edges.to + "', expected '" + kLayerNames[i] + "' to '" +
                                   kLayerNames[i + 1] + "'");
            }
            if (!entry.contains("rows") || !entry.at("rows").is_number_unsigned() ||
                !entry.contains("cols") || !entry.at("cols").is_number_unsigned()) {
                throw AdvisorError("advisor: activation_layout.json edge '" + edges.from +
                                   "' -> '" + edges.to + "' has no unsigned rows/cols");
            }
            edges.rows = entry.at("rows").get<std::size_t>();
            edges.cols = entry.at("cols").get<std::size_t>();
            if (edges.rows != parsed.layers[i + 1].size ||
                edges.cols != parsed.layers[i].size) {
                throw AdvisorError("advisor: activation_layout.json edge '" + edges.from +
                                   "' -> '" + edges.to + "' is " + std::to_string(edges.rows) +
                                   "x" + std::to_string(edges.cols) + ", expected " +
                                   std::to_string(parsed.layers[i + 1].size) + "x" +
                                   std::to_string(parsed.layers[i].size) +
                                   " for the layers it joins");
            }
            if (!entry.contains("weights") || !entry.at("weights").is_array() ||
                entry.at("weights").size() != edges.rows) {
                throw AdvisorError("advisor: activation_layout.json edge '" + edges.from +
                                   "' -> '" + edges.to + "' has no " +
                                   std::to_string(edges.rows) + "-row 'weights' matrix");
            }
            edges.weights.resize(edges.rows * edges.cols);
            for (std::size_t r = 0; r < edges.rows; ++r) {
                const json& row = entry.at("weights")[r];
                if (!row.is_array() || row.size() != edges.cols) {
                    throw AdvisorError("advisor: activation_layout.json edge '" + edges.from +
                                       "' -> '" + edges.to + "' row " + std::to_string(r) +
                                       " is not " + std::to_string(edges.cols) + " wide");
                }
                for (std::size_t c = 0; c < edges.cols; ++c) {
                    const double value = row[c].get<double>();
                    if (!std::isfinite(value)) {
                        throw AdvisorError("advisor: activation_layout.json edge '" +
                                           edges.from + "' -> '" + edges.to +
                                           "' holds a non-finite weight at (" +
                                           std::to_string(r) + ", " + std::to_string(c) + ")");
                    }
                    // float32 on purpose: these are the drawing's line widths,
                    // and the weights themselves are float32 in the graph.
                    edges.weights[r * edges.cols + c] = static_cast<float>(value);
                }
            }
            parsed.edges.push_back(std::move(edges));
        }
    } catch (const AdvisorError& error) {
        activation_note = std::string(error.what()) + remedy;
        return;
    } catch (const std::exception& error) {
        // Anything nlohmann raises on a payload that parses but is not shaped
        // like the schema. Prefixed, because those messages name a JSON type
        // and nothing else, and a user needs to know which file to re-export.
        activation_note =
            "advisor: activation_layout.json is malformed: " + std::string(error.what()) +
            remedy;
        return;
    }

    layout = std::move(parsed);
    activations_available = true;
    activation_note.clear();
}

bool Advisor::has_activations() const { return impl_->activations_available; }

const NetworkLayout& Advisor::layout() const { return impl_->layout; }

AdvisorExplanation Advisor::explain(const pipeline::CaseFeatures& features,
                                    double max_dof) const {
    return explain(to_columns(features), max_dof);
}

AdvisorExplanation Advisor::explain(const FeatureColumns& columns, double max_dof) const {
    // Throws, unlike `recommend`, and deliberately: there is no honest reduced
    // answer here. A caller asking what the network did cannot be handed a
    // decision with empty activations and left to guess whether the network was
    // idle or the model directory was stale, so the reason is reported instead.
    if (!impl_->activations_available) {
        throw AdvisorError(impl_->activation_note);
    }
    AdvisorExplanation explanation;
    explanation.gate_threshold = impl_->gate_threshold;
    explanation.decision = decide(columns, max_dof, &explanation);
    return explanation;
}

ActivationTaps Advisor::taps(const FeatureColumns& columns) const {
    // Same guard and same reason as `explain`: no honest reduced answer.
    if (!impl_->activations_available) {
        throw AdvisorError(impl_->activation_note);
    }
    // Routed through the one `forward`, so this cannot become a second way of
    // reading the graph that disagrees with the one the decision uses.
    ActivationFrame frame;
    // The head outputs are already carried on the frame; the return value is
    // the unpacked form the chooser wants and this caller does not.
    static_cast<void>(impl_->forward(columns, &frame));
    return {std::move(frame.input), std::move(frame.fc1), std::move(frame.fc2),
            std::move(frame.heads)};
}

} // namespace polymesh::advisor
