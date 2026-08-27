import {Viewport} from "./viewport.js";
import {NetworkPanel} from "./network.js";

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => [...document.querySelectorAll(selector)];
const kDebug = new URLSearchParams(location.search).get("debug") === "1";
const kSupportedExtensions = new Set(["step", "stp", "brep", "stl"]);
const kMaterials = {
    steel: {E: 200e9, nu: 0.30},
    aluminum: {E: 68.9e9, nu: 0.33},
};

const state = {
    part: null,
    job: null,
    eventSource: null,
    reconnects: 0,
    done: false,
    passes: new Map(),
    lastPass: null,
    result: null,
    boxes: {fixture: null, load: null},
};

let viewport = null;
let network = null;
try {
    viewport = new Viewport($("#viewport"));
    network = new NetworkPanel($("#network"));
} catch (error) {
    setMessage($("#run-message"), error.message);
    $("#run").disabled = true;
}

function debugEvent(name, data) {
    if (kDebug) console.log(`[PolyMesh SSE] ${name}`, data);
}

function formatNumber(value, digits = 4) {
    if (!Number.isFinite(Number(value))) return "—";
    return new Intl.NumberFormat(undefined, {maximumSignificantDigits: digits}).format(Number(value));
}

function formatInteger(value) {
    if (!Number.isFinite(Number(value))) return "—";
    return new Intl.NumberFormat().format(Math.trunc(Number(value)));
}

function setMessage(element, text, isError = false) {
    element.textContent = text;
    element.classList.toggle("message-error", isError);
}

async function api(path, options = {}) {
    let response;
    try {
        response = await fetch(path, options);
    } catch (_) {
        throw new Error("PolyMesh server is unreachable.");
    }
    let payload = null;
    try { payload = await response.json(); } catch (_) { /* The status still identifies the failure. */ }
    if (!response.ok || payload?.ok === false) {
        throw new Error(payload?.error || `Request failed with HTTP ${response.status}.`);
    }
    return payload;
}

async function loadHealth() {
    const health = $("#health");
    try {
        const data = await api("/api/health");
        health.dataset.state = "ok";
        health.textContent = `Ready · ${formatInteger(data.threads)} threads`;
    } catch (error) {
        health.dataset.state = "error";
        health.textContent = error.message;
    }
}

async function loadExamples() {
    const row = $("#examples");
    try {
        const data = await api("/api/examples");
        row.replaceChildren();
        if (!data.examples?.length) {
            const empty = document.createElement("span");
            empty.className = "muted";
            empty.textContent = "No bundled examples reported by the server.";
            row.append(empty);
            return;
        }
        for (const example of data.examples) {
            const button = document.createElement("button");
            button.type = "button";
            button.className = "example-button";
            button.textContent = example.name;
            button.addEventListener("click", () => loadExample(example.id, button));
            row.append(button);
        }
    } catch (error) {
        row.replaceChildren();
        const message = document.createElement("span");
        message.className = "muted";
        message.textContent = error.message;
        row.append(message);
    }
}

async function loadExample(id, button) {
    if (state.job) {
        showUploadError("Stop the current study before loading different geometry.");
        return;
    }
    clearUploadError();
    button.disabled = true;
    setMessage($("#run-message"), "Loading example geometry.");
    try {
        const data = await api(`/api/parts/example/${encodeURIComponent(id)}`, {method: "POST"});
        acceptPart(data.part);
    } catch (error) {
        showUploadError(error.message);
        setMessage($("#run-message"), error.message, true);
    } finally {
        button.disabled = false;
    }
}

async function uploadFile(file) {
    if (state.job) {
        showUploadError("Stop the current study before loading different geometry.");
        return;
    }
    clearUploadError();
    if (!file) return;
    const extension = file.name.split(".").pop()?.toLowerCase();
    if (!kSupportedExtensions.has(extension)) {
        showUploadError("Unsupported geometry. Choose a .step, .stp, .brep, or .stl file.");
        return;
    }
    setMessage($("#run-message"), `Uploading ${file.name}.`);
    try {
        const data = await api("/api/parts", {
            method: "POST",
            headers: {"Content-Type": "application/octet-stream", "X-PolyMesh-Filename": file.name},
            body: file,
        });
        acceptPart(data.part);
    } catch (error) {
        showUploadError(error.message);
        setMessage($("#run-message"), error.message, true);
    } finally {
        $("#part-file").value = "";
    }
}

function showUploadError(text) {
    const element = $("#upload-error");
    element.textContent = text;
    element.hidden = false;
}
function clearUploadError() { $("#upload-error").hidden = true; }

function acceptPart(part) {
    if (!part?.surface || !Array.isArray(part.bbox_min) || !Array.isArray(part.bbox_max)) {
        showUploadError("The server returned incomplete geometry metadata.");
        return;
    }
    state.part = part;
    state.result = null;
    state.passes.clear();
    state.lastPass = null;
    resetResults();
    network?.reset();
    $("#drop-zone strong").textContent = part.name;
    $("#drop-zone span").textContent = `${part.kind.toUpperCase()} geometry loaded`;
    $("#part-triangles").textContent = formatInteger(part.triangles);
    $("#part-regions").textContent = formatInteger(part.regions);
    const formatPoint = (point) => point.map((value) => formatNumber(value, 5)).join(", ");
    $("#part-bounds").textContent = `[${formatPoint(part.bbox_min)}] to [${formatPoint(part.bbox_max)}] model units`;
    $("#part-facts").hidden = false;
    $("#element-size").value = String(part.suggested_h);
    $("#element-size").disabled = false;
    $("#run").disabled = !viewport;
    $("#fixture-box").disabled = false;
    $("#load-box").disabled = false;
    buildBoxEditors(part.bbox_min, part.bbox_max);
    viewport?.setPart(part.surface, part.bbox_min, part.bbox_max);
    selectField("cad");
    $("#stage").textContent = part.name;
    $("#progress-copy").textContent = `${formatInteger(part.triangles)} triangles · ${formatInteger(part.regions)} regions`;
    setMessage($("#run-message"), "Geometry is ready. Define boundary boxes, then run the study.");
}

function buildBoxEditors(min, max) {
    const span = max.map((value, i) => value - min[i]);
    state.boxes.fixture = [min[0], min[1], min[2], min[0] + span[0] * 0.05, max[1], max[2]];
    state.boxes.load = [max[0] - span[0] * 0.05, min[1], min[2], max[0], max[1], max[2]];
    for (const kind of ["fixture", "load"]) {
        const container = $(`.axis-grid[data-box="${kind}"]`);
        container.replaceChildren();
        ["X", "Y", "Z"].forEach((axis, index) => {
            const label = document.createElement("span");
            label.className = "axis-name";
            label.textContent = axis;
            container.append(label);
            for (const [bound, offset] of [["min", index], ["max", index + 3]]) {
                const wrapper = document.createElement("label");
                wrapper.textContent = bound;
                const input = document.createElement("input");
                input.type = "number";
                input.step = "any";
                input.min = String(min[index]);
                input.max = String(max[index]);
                input.value = String(state.boxes[kind][offset]);
                input.dataset.kind = kind;
                input.dataset.offset = String(offset);
                input.addEventListener("input", updateBox);
                wrapper.append(input);
                container.append(wrapper);
            }
        });
    }
    updateBoxDisplay();
}

function updateBox(event) {
    const {kind, offset} = event.target.dataset;
    const value = Number(event.target.value);
    if (Number.isFinite(value)) state.boxes[kind][Number(offset)] = value;
    updateBoxDisplay();
}

function normalizedBox(kind) {
    const box = state.boxes[kind];
    if (!box) return null;
    return [
        Math.min(box[0], box[3]), Math.min(box[1], box[4]), Math.min(box[2], box[5]),
        Math.max(box[0], box[3]), Math.max(box[1], box[4]), Math.max(box[2], box[5]),
    ];
}

function updateBoxDisplay() {
    const fixture = normalizedBox("fixture");
    const load = normalizedBox("load");
    if (fixture && load) viewport?.setBoxes(fixture, load);
}

function studyPayload() {
    const h = Number($("#element-size").value);
    const E = Number($("#youngs").value);
    const nu = Number($("#poisson").value);
    const etaTarget = Number($("#eta-target").value);
    const adaptPasses = Number($("#adapt-passes").value);
    const skinLayers = Number($("#skin-layers").value);
    if (!(h > 0)) throw new Error("Element size must be greater than zero model units.");
    if (!(E > 0)) throw new Error("Young's modulus must be greater than zero Pa.");
    if (!(nu > -1 && nu < 0.5)) throw new Error("Poisson ratio must be between -1 and 0.5.");
    if (!(etaTarget >= 0)) throw new Error("Eta target must be non-negative.");
    // Domains match the pipeline, not a guess: `--adapt 0` is how the CLI turns
    // adaptivity off, and `pipeline::volume_mesh` takes skin_layers >= 1 (the
    // backend rejects 0). Validating a narrower range here made the default
    // configuration unable to start.
    if (!Number.isInteger(adaptPasses) || adaptPasses < 0)
        throw new Error("Adaptive passes must be a non-negative integer (0 turns adaptivity off).");
    if (!Number.isInteger(skinLayers) || skinLayers < 1)
        throw new Error("Skin layers must be a positive integer.");
    const force = [Number($("#force-x").value), Number($("#force-y").value), Number($("#force-z").value)];
    if (!force.every(Number.isFinite)) throw new Error("Load force components must be finite values in newtons.");
    return {
        part: state.part.id,
        kind: "solve",
        h, E, nu,
        mesher: $("#mesher").value,
        adapt_passes: adaptPasses,
        eta_target: etaTarget,
        skin_layers: skinLayers,
        feature_grading: $("#feature-grading").checked,
        fixtures: [normalizedBox("fixture")],
        loads: [{box: normalizedBox("load"), force}],
    };
}

async function runStudy() {
    if (!state.part || state.job) return;
    let payload;
    try { payload = studyPayload(); }
    catch (error) { setMessage($("#run-message"), error.message, true); return; }
    resetRunState();
    setRunning(true);
    $("#stop").disabled = true;
    state.job = "submitting";
    setMessage($("#run-message"), "Submitting study.");
    $("#stage").textContent = "Study queued";
    $("#progress-copy").textContent = "Waiting for the mesher.";
    try {
        const data = await api("/api/jobs", {method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(payload)});
        state.job = data.job;
        $("#stop").disabled = false;
        $("#export-vtu").href = `/api/jobs/${encodeURIComponent(state.job)}/result.vtu`;
        openEvents(state.job);
    } catch (error) {
        state.job = null;
        setRunning(false);
        setMessage($("#run-message"), error.message, true);
        $("#stage").textContent = "Study not started";
        $("#progress-copy").textContent = error.message;
    }
}

function resetRunState() {
    state.done = false;
    state.reconnects = 0;
    state.passes.clear();
    state.lastPass = null;
    state.result = null;
    network?.reset();
    resetResults();
}

function setRunning(running) {
    $("#run").hidden = running;
    $("#stop").hidden = !running;
    $("#run").disabled = running || !state.part || !viewport;
    $$(".setup-section input, .setup-section select, .example-button").forEach((control) => {
        if (control.id !== "part-file") control.disabled = running;
    });
    $("#part-file").disabled = running;
}

async function stopStudy() {
    if (!state.job || state.done) return;
    $("#stop").disabled = true;
    setMessage($("#run-message"), "Cancellation requested. Waiting for the solver to stop.");
    try {
        await api(`/api/jobs/${encodeURIComponent(state.job)}`, {method: "DELETE"});
    } catch (error) {
        $("#stop").disabled = false;
        setMessage($("#run-message"), error.message, true);
    }
}

function openEvents(job) {
    state.eventSource?.close();
    const source = new EventSource(`/api/jobs/${encodeURIComponent(job)}/events`);
    state.eventSource = source;
    const names = ["hello", "advisor", "mesh", "progress", "pass", "result", "note", "done"];
    for (const name of names) {
        source.addEventListener(name, (event) => {
            let data;
            try { data = JSON.parse(event.data); }
            catch (_) {
                finishJob("failed", `The server sent invalid ${name} event data.`);
                return;
            }
            debugEvent(name, data);
            handleEvent(name, data);
        });
    }
    source.onerror = () => {
        source.close();
        if (state.done || state.eventSource !== source) return;
        if (state.reconnects < 1) {
            state.reconnects += 1;
            setMessage($("#run-message"), "Event stream dropped. Reconnecting once to replay the buffered run.");
            setTimeout(() => { if (!state.done && state.job === job) openEvents(job); }, 750);
        } else {
            finishJob("failed", "The event stream disconnected twice. The run may still be active on the server.");
        }
    };
}

function handleEvent(name, data) {
    if (name === "hello") {
        setMessage($("#run-message"), `Running ${data.mesher} study at h ${formatNumber(data.h, 5)} model units.`);
        return;
    }
    if (name === "advisor") {
        network?.setData(data);
        $("#stage").textContent = "Advisor deliberation";
        $("#progress-copy").textContent = "Playing activations from the decision's real forward pass.";
        return;
    }
    if (name === "mesh") {
        try { viewport?.setMesh(data.cells); }
        catch (error) { finishJob("failed", `Mesh display failed: ${error.message}`); return; }
        network?.setMeshStage(data.index);
        selectField("mesh");
        $("#stage").textContent = data.stage;
        const meshFacts = [
            `${formatInteger(data.n_elems)} elements`,
            `${formatInteger(data.n_nodes)} nodes`,
            `pass ${formatInteger(data.pass)}`,
        ];
        const emitted = Number(data.emitted_elems);
        if (Number.isFinite(emitted) && emitted < Number(data.n_elems)) {
            meshFacts.push(`${formatInteger(emitted)} emitted for live view`);
        }
        $("#progress-copy").textContent = meshFacts.join(" · ");
        return;
    }
    if (name === "progress") {
        network?.setProgress(data);
        $("#stage").textContent = data.phase;
        const facts = [];
        if (Number.isFinite(Number(data.phase_frac))) facts.push(`${formatNumber(Number(data.phase_frac) * 100, 3)}%`);
        if (Number.isFinite(Number(data.n_elems)) && Number(data.n_elems) > 0) facts.push(`${formatInteger(data.n_elems)} elements`);
        if (Number.isFinite(Number(data.n_nodes)) && Number(data.n_nodes) > 0) facts.push(`${formatInteger(data.n_nodes)} nodes`);
        if (Number.isFinite(Number(data.cg_iter)) && Number(data.cg_iter) > 0) facts.push(`CG ${formatInteger(data.cg_iter)} · residual ${formatNumber(data.cg_resid, 4)}`);
        if (Number.isFinite(Number(data.elapsed_ms))) facts.push(`${formatInteger(data.elapsed_ms)} ms elapsed`);
        $("#progress-copy").textContent = facts.length ? facts.join(" · ") : "Waiting for the mesher.";
        return;
    }
    if (name === "pass") {
        state.passes.set(Number(data.pass), data);
        state.lastPass = data;
        renderPasses();
        updatePassMetrics(data);
        return;
    }
    if (name === "result") {
        state.result = data;
        try { viewport?.setResult(data.surface, data); }
        catch (error) { finishJob("failed", `Result display failed: ${error.message}`); return; }
        renderResult(data);
        selectField("von_mises");
        return;
    }
    if (name === "note") {
        if (data.text) appendNote(data.text);
        return;
    }
    if (name === "done") finishJob(data.state, data.message, data.elapsed_ms);
}

function finishJob(status, message, elapsedMs = null) {
    if (state.done) return;
    state.done = true;
    state.eventSource?.close();
    state.eventSource = null;
    network?.retire();
    setRunning(false);
    $("#stop").disabled = false;
    const elapsed = Number.isFinite(Number(elapsedMs)) ? ` · ${formatInteger(elapsedMs)} ms` : "";
    if (status === "done") {
        setMessage($("#run-message"), `Study complete${elapsed}.`);
        $("#stage").textContent = "done";
        $("#progress-copy").textContent = message || "The solve completed.";
        if (state.result) {
            $("#export-vtu").classList.remove("disabled");
            $("#export-vtu").setAttribute("aria-disabled", "false");
        }
    } else if (status === "cancelled") {
        setMessage($("#run-message"), `Study cancelled${elapsed}.`);
        $("#stage").textContent = "cancelled";
        $("#progress-copy").textContent = message || "The server cancelled the run.";
    } else {
        setMessage($("#run-message"), message || "The study failed.", true);
        $("#stage").textContent = "failed";
        $("#progress-copy").textContent = message || "The server reported a failed run.";
    }
    state.job = null;
}

function renderResult(data) {
    $("#metric-nodes").textContent = formatInteger(data.n_nodes);
    $("#metric-elements").textContent = formatInteger(data.n_elems);
    $("#metric-dof").textContent = formatInteger(data.dof);
    $("#metric-stress").textContent = formatNumber(Number(data.max_von_mises) / 1e6, 5);
    $("#metric-displacement").textContent = formatNumber(Number(data.max_displacement) * 1e3, 5);
    $("#metric-eta").textContent = formatNumber(data.global_eta, 5);
    $("#deform").disabled = false;
    $$("#field-switcher button[data-field='von_mises'], #field-switcher button[data-field='displacement'], #field-switcher button[data-field='eta']").forEach((button) => { button.disabled = false; });
    if (data.mesh_note) appendNote(data.mesh_note);
    if (data.solver_note) appendNote(data.solver_note);
    if (state.lastPass) updatePassMetrics(state.lastPass);
}

function updatePassMetrics(pass) {
    $("#metric-solver").textContent = pass.solve_method || "—";
    const mesh = Number(pass.mesh_ms);
    const solve = Number(pass.solve_ms);
    $("#metric-timing").textContent = Number.isFinite(mesh) && Number.isFinite(solve) ? `${formatNumber(mesh, 4)} / ${formatNumber(solve, 4)}` : "—";
}

function appendNote(text) {
    const note = document.createElement("p");
    note.textContent = text;
    $("#run-notes").append(note);
}

function renderPasses() {
    const passes = [...state.passes.values()].sort((a, b) => Number(a.pass) - Number(b.pass));
    const panel = $("#convergence-panel");
    panel.hidden = passes.length < 2;
    const body = $("#convergence-body");
    body.replaceChildren();
    for (const pass of passes) {
        const row = document.createElement("tr");
        const values = [
            formatInteger(pass.pass), formatInteger(pass.n_elems), formatInteger(pass.dof),
            formatNumber(pass.global_eta, 5), `${formatNumber(pass.mesh_ms, 4)} / ${formatNumber(pass.solve_ms, 4)}`,
        ];
        for (const value of values) {
            const cell = document.createElement("td");
            cell.textContent = value;
            row.append(cell);
        }
        body.append(row);
    }
}

function resetResults() {
    for (const id of ["metric-nodes", "metric-elements", "metric-dof", "metric-stress", "metric-displacement", "metric-eta", "metric-solver", "metric-timing"]) $("#" + id).textContent = "—";
    $("#run-notes").replaceChildren();
    $("#convergence-panel").hidden = true;
    $("#convergence-body").replaceChildren();
    $("#export-vtu").classList.add("disabled");
    $("#export-vtu").setAttribute("aria-disabled", "true");
    $("#deform").disabled = true;
    $("#colorbar").hidden = true;
    $$("#field-switcher button[data-field='von_mises'], #field-switcher button[data-field='displacement'], #field-switcher button[data-field='eta']").forEach((button) => { button.disabled = true; });
}

function selectField(field) {
    const button = $(`#field-switcher button[data-field="${field}"]`);
    if (!button || button.disabled) return;
    $$("#field-switcher button").forEach((item) => item.setAttribute("aria-pressed", String(item === button)));
    viewport?.setMode(field);
    updateColorbar(field);
}

// Ported verbatim from apps/gui/colormap.hpp. The scalar shader uses the same
// equations; these breakpoints keep the HTML legend honest to those equations.
function feaColor(t) {
    const value = Math.max(0, Math.min(1, t));
    const r = Math.max(0, Math.min(1, Math.min(4 * value - 2, 4 - 4 * value) + 1));
    const g = Math.max(0, Math.min(1, Math.min(4 * value, 3.4 - 3 * value)));
    const b = Math.max(0, Math.min(1, 2 - 4 * value));
    return [value > 0.75 ? 1 : r * 0.9, g * 0.85, b];
}

function feaGradient() {
    const stops = [0, 0.25, 0.5, 0.75, 0.750001, 0.8, 1];
    return `linear-gradient(to top, ${stops.map((t) => {
        const color = feaColor(t).map((component) => Math.round(component * 255));
        return `rgb(${color.join(" ")}) ${t * 100}%`;
    }).join(", ")})`;
}

function updateColorbar(field) {
    const bar = $("#colorbar");
    const settings = {
        von_mises: {unit: "MPa", factor: 1e-6},
        displacement: {unit: "mm", factor: 1e3},
        eta: {unit: "eta", factor: 1},
    }[field];
    const range = settings ? viewport?.fieldRange(field) : null;
    bar.hidden = !range;
    if (!range) return;
    $("#colorbar-min").textContent = formatNumber(range[0] * settings.factor, 4);
    $("#colorbar-max").textContent = formatNumber(range[1] * settings.factor, 4);
    $("#colorbar-unit").textContent = settings.unit;
    $(".color-ramp").style.background = feaGradient();
}

function installControls() {
    const input = $("#part-file");
    input.addEventListener("change", () => uploadFile(input.files?.[0]));
    const drop = $("#drop-zone");
    for (const name of ["dragenter", "dragover"]) drop.addEventListener(name, (event) => { event.preventDefault(); drop.classList.add("dragging"); });
    for (const name of ["dragleave", "drop"]) drop.addEventListener(name, (event) => { event.preventDefault(); drop.classList.remove("dragging"); });
    drop.addEventListener("drop", (event) => uploadFile(event.dataTransfer?.files?.[0]));
    drop.addEventListener("keydown", (event) => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); input.click(); } });
    $("#material").addEventListener("change", (event) => {
        const material = kMaterials[event.target.value];
        if (!material) return;
        $("#youngs").value = String(material.E);
        $("#poisson").value = String(material.nu);
    });
    for (const id of ["youngs", "poisson"]) {
        $("#" + id).addEventListener("input", () => { $("#material").value = "custom"; });
    }
    $("#run").addEventListener("click", runStudy);
    $("#stop").addEventListener("click", stopStudy);
    $("#fit").addEventListener("click", () => viewport?.fit());
    $("#wireframe").addEventListener("change", (event) => viewport?.setWireframe(event.target.checked));
    $("#ghost").addEventListener("change", (event) => viewport?.setGhost(event.target.checked));
    $("#field-switcher").addEventListener("click", (event) => {
        const button = event.target.closest("button[data-field]");
        if (button) selectField(button.dataset.field);
    });
    $("#deform").addEventListener("change", (event) => {
        const custom = event.target.value === "custom";
        $("#deform-custom-wrap").hidden = !custom;
        viewport?.setDeformation(event.target.value, Number($("#deform-custom").value));
    });
    $("#deform-custom").addEventListener("input", (event) => viewport?.setDeformation("custom", Number(event.target.value)));
    $("#export-vtu").addEventListener("click", (event) => {
        if (event.currentTarget.getAttribute("aria-disabled") === "true") event.preventDefault();
    });
}

installControls();
loadHealth();
loadExamples();
