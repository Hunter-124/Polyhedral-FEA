const kArrivalBand = 0.045;
const kRevealShrink = 0.14;
const kRevealSeconds = 1.4;
const kReducedMotion = matchMedia("(prefers-reduced-motion: reduce)");

export function decodeFloat32(encoded, expected = null) {
    if (!encoded) return new Float32Array();
    const raw = atob(encoded);
    const bytes = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; ++i) bytes[i] = raw.charCodeAt(i);
    const count = Math.floor(bytes.byteLength / 4);
    const values = new Float32Array(count);
    const view = new DataView(bytes.buffer);
    for (let i = 0; i < count; ++i) values[i] = view.getFloat32(i * 4, true);
    if (expected !== null && count !== expected) {
        throw new Error(`Expected ${expected} Float32 values, received ${count}`);
    }
    return values;
}

function shader(gl, type, source) {
    const value = gl.createShader(type);
    gl.shaderSource(value, source);
    gl.compileShader(value);
    if (!gl.getShaderParameter(value, gl.COMPILE_STATUS)) {
        const reason = gl.getShaderInfoLog(value);
        gl.deleteShader(value);
        throw new Error(`WebGL shader failed: ${reason}`);
    }
    return value;
}

function program(gl, vertex, fragment) {
    const value = gl.createProgram();
    gl.attachShader(value, shader(gl, gl.VERTEX_SHADER, vertex));
    gl.attachShader(value, shader(gl, gl.FRAGMENT_SHADER, fragment));
    gl.linkProgram(value);
    if (!gl.getProgramParameter(value, gl.LINK_STATUS)) {
        const reason = gl.getProgramInfoLog(value);
        gl.deleteProgram(value);
        throw new Error(`WebGL link failed: ${reason}`);
    }
    return value;
}

function identity() {
    return new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
}

function multiply(a, b) {
    const out = new Float32Array(16);
    for (let col = 0; col < 4; ++col) {
        for (let row = 0; row < 4; ++row) {
            let sum = 0;
            for (let k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[col * 4 + k];
            out[col * 4 + row] = sum;
        }
    }
    return out;
}

function perspective(fov, aspect, near, far) {
    const f = 1 / Math.tan(fov / 2);
    const out = new Float32Array(16);
    out[0] = f / aspect;
    out[5] = f;
    out[10] = (far + near) / (near - far);
    out[11] = -1;
    out[14] = 2 * far * near / (near - far);
    return out;
}

function normalize(v) {
    const n = Math.hypot(v[0], v[1], v[2]) || 1;
    return [v[0] / n, v[1] / n, v[2] / n];
}
function cross(a, b) {
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}
function dot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
function sub(a, b) { return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]; }

function lookAt(eye, target, up) {
    const z = normalize(sub(eye, target));
    const x = normalize(cross(up, z));
    const y = cross(z, x);
    const out = identity();
    out[0] = x[0]; out[1] = y[0]; out[2] = z[0];
    out[4] = x[1]; out[5] = y[1]; out[6] = z[1];
    out[8] = x[2]; out[9] = y[2]; out[10] = z[2];
    out[12] = -dot(x, eye); out[13] = -dot(y, eye); out[14] = -dot(z, eye);
    return out;
}

function cssColor(name, fallback) {
    const raw = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
    const match = /^#([0-9a-f]{6})$/i.exec(raw);
    if (!match) return fallback;
    const n = Number.parseInt(match[1], 16);
    return [(n >> 16) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255];
}

function finiteRange(values) {
    let min = Infinity;
    let max = -Infinity;
    for (const value of values) {
        if (!Number.isFinite(value)) continue;
        min = Math.min(min, value);
        max = Math.max(max, value);
    }
    if (!Number.isFinite(min)) return [0, 0];
    return [min, max];
}

function boxLines(box) {
    const [x0, y0, z0, x1, y1, z1] = box;
    const p = [[x0,y0,z0],[x1,y0,z0],[x1,y1,z0],[x0,y1,z0],[x0,y0,z1],[x1,y0,z1],[x1,y1,z1],[x0,y1,z1]];
    const edges = [[0,1],[1,2],[2,3],[3,0],[4,5],[5,6],[6,7],[7,4],[0,4],[1,5],[2,6],[3,7]];
    return new Float32Array(edges.flatMap(([a, b]) => [...p[a], ...p[b]]));
}

const kSurfaceVertex = `#version 300 es
precision highp float;
layout(location=0) in vec3 a_position;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec3 a_displacement;
layout(location=3) in float a_scalar;
uniform mat4 u_mvp;
uniform float u_deform;
uniform float u_min;
uniform float u_max;
out vec3 v_normal;
out float v_scalar;
void main() {
    vec3 p = a_position + u_deform * a_displacement;
    gl_Position = u_mvp * vec4(p, 1.0);
    v_normal = a_normal;
    v_scalar = u_max > u_min ? clamp((a_scalar - u_min) / (u_max - u_min), 0.0, 1.0) : 0.0;
}`;

const kSurfaceFragment = `#version 300 es
precision highp float;
in vec3 v_normal;
in float v_scalar;
uniform int u_mode;
uniform vec3 u_base;
uniform float u_alpha;
out vec4 out_color;
vec3 fea(float t) {
    float r = clamp(min(4.0*t-2.0, 4.0-4.0*t)+1.0, 0.0, 1.0);
    float g = clamp(min(4.0*t, 3.4-3.0*t), 0.0, 1.0);
    float b = clamp(2.0-4.0*t, 0.0, 1.0);
    return vec3(t > 0.75 ? 1.0 : r*0.9, g*0.85, b);
}
void main() {
    vec3 n = normalize(gl_FrontFacing ? v_normal : -v_normal);
    if (length(v_normal) < 0.01) n = normalize(cross(dFdx(gl_FragCoord.xyz), dFdy(gl_FragCoord.xyz)));
    float light = 0.35 + 0.65 * abs(dot(n, normalize(vec3(0.35, -0.5, 0.79))));
    vec3 color = u_mode == 0 ? u_base : fea(v_scalar);
    out_color = vec4(color * light, u_alpha);
}`;

const kRevealVertex = `#version 300 es
precision highp float;
layout(location=0) in vec3 a_position;
layout(location=1) in vec3 a_centroid;
layout(location=2) in float a_index;
layout(location=3) in vec3 a_color;
uniform mat4 u_mvp;
uniform float u_reveal;
uniform float u_shrink;
uniform float u_arrival;
out float v_index;
out vec3 v_color;
void main() {
    // Only the narrow arrival band is contracted. Everything behind it is its
    // measured element geometry, while unreported cells remain discarded.
    float arrival = smoothstep(u_reveal-u_arrival, u_reveal, a_index);
    float scale = mix(1.0, u_shrink, arrival);
    vec3 p = a_centroid + (a_position-a_centroid)*scale;
    gl_Position = u_mvp*vec4(p, 1.0);
    v_index = a_index;
    v_color = a_color;
}`;

const kRevealFragment = `#version 300 es
precision highp float;
in float v_index;
in vec3 v_color;
uniform float u_reveal;
uniform float u_arrival;
out vec4 out_color;
void main() {
    if (v_index >= u_reveal) discard;
    float w = smoothstep(u_reveal-u_arrival, u_reveal, v_index);
    vec3 hot = mix(vec3(1.00,0.72,0.30), vec3(1.00,0.97,0.88), w);
    out_color = vec4(mix(v_color, hot, w), 1.0);
}`;

const kLineVertex = `#version 300 es
precision highp float;
layout(location=0) in vec3 a_position;
uniform mat4 u_mvp;
void main() { gl_Position = u_mvp * vec4(a_position, 1.0); }`;
const kLineFragment = `#version 300 es
precision highp float;
uniform vec4 u_color;
out vec4 out_color;
void main() { out_color = u_color; }`;

export class Viewport {
    constructor(canvas) {
        this.canvas = canvas;
        this.gl = canvas.getContext("webgl2", {antialias: true, alpha: false});
        if (!this.gl) throw new Error("WebGL 2 is required to display PolyMesh geometry.");
        const gl = this.gl;
        this.surfaceProgram = program(gl, kSurfaceVertex, kSurfaceFragment);
        this.revealProgram = program(gl, kRevealVertex, kRevealFragment);
        this.lineProgram = program(gl, kLineVertex, kLineFragment);
        this.surface = null;
        this.mesh = null;
        this.result = null;
        this.mode = "cad";
        this.wireframe = true;
        this.ghost = false;
        this.deformMode = "auto";
        this.customDeform = 10;
        this.boxes = [];
        this.bbox = [[-0.5, -0.5, -0.5], [0.5, 0.5, 0.5]];
        this.target = [0, 0, 0];
        this.distance = 3;
        this.yaw = 0.7;
        this.pitch = 0.5;
        this.pointer = null;
        this.touches = new Map();
        this.reveal = 1;
        this.revealStart = 0;
        this.installInput();
        this.resizeObserver = new ResizeObserver(() => this.resize());
        this.resizeObserver.observe(canvas.parentElement);
        this.resize();
        requestAnimationFrame((time) => this.draw(time));
    }

    makeBuffer(values) {
        const buffer = this.gl.createBuffer();
        this.gl.bindBuffer(this.gl.ARRAY_BUFFER, buffer);
        this.gl.bufferData(this.gl.ARRAY_BUFFER, values, this.gl.STATIC_DRAW);
        return buffer;
    }

    deleteBuffers(value, keys) {
        if (!value) return;
        for (const key of keys) {
            if (value[key]) this.gl.deleteBuffer(value[key]);
        }
    }

    deleteSurface(value) {
        if (!value) return;
        this.deleteBuffers(value, ["positions", "normals", "edges", "displacement"]);
        for (const buffer of Object.values(value.fieldBuffers || {})) {
            this.gl.deleteBuffer(buffer);
        }
    }

    decodeSurface(data) {
        const n = Number(data.n_verts) || 0;
        return {
            count: n,
            positions: this.makeBuffer(decodeFloat32(data.positions_b64, n * 3)),
            normals: data.normals_b64
                ? this.makeBuffer(decodeFloat32(data.normals_b64, n * 3))
                : null,
            edges: data.edges_b64 ? this.makeBuffer(decodeFloat32(data.edges_b64)) : null,
            edgeCount: Number(data.n_edge_verts) || 0,
            displacement: data.disp_b64 ? this.makeBuffer(decodeFloat32(data.disp_b64, n * 3)) : null,
            fields: {
                von_mises: data.von_mises_b64 ? decodeFloat32(data.von_mises_b64, n) : null,
                displacement: data.u_mag_b64 ? decodeFloat32(data.u_mag_b64, n) : null,
                eta: data.eta_b64 ? decodeFloat32(data.eta_b64, n) : null,
            },
            fieldBuffers: {},
        };
    }

    setPart(surface, bboxMin, bboxMax) {
        this.deleteSurface(this.surface);
        this.surface = this.decodeSurface(surface);
        this.bbox = [bboxMin.slice(), bboxMax.slice()];
        this.mode = "cad";
        this.fit();
    }

    setMesh(data) {
        this.deleteBuffers(
            this.mesh,
            ["positions", "centroids", "indices", "colors", "edges"],
        );
        const n = Number(data.n_verts) || 0;
        this.mesh = {
            count: n,
            positions: this.makeBuffer(decodeFloat32(data.positions_b64, n * 3)),
            centroids: this.makeBuffer(decodeFloat32(data.centroids_b64, n * 3)),
            indices: this.makeBuffer(decodeFloat32(data.index_b64, n)),
            colors: this.makeBuffer(decodeFloat32(data.color_b64, n * 3)),
            edges: this.makeBuffer(decodeFloat32(data.edges_b64 || "")),
            edgeCount: Number(data.n_edge_verts) || 0,
        };
        this.mode = "mesh";
        this.reveal = kReducedMotion.matches ? 1 : 0;
        this.revealStart = performance.now();
    }

    setResult(surface, summary) {
        this.deleteSurface(this.result);
        this.result = this.decodeSurface(surface);
        this.result.summary = summary;
        for (const [name, values] of Object.entries(this.result.fields)) {
            if (!values) continue;
            this.result.fieldBuffers[name] = this.makeBuffer(values);
            this.result[`${name}Range`] = finiteRange(values);
        }
    }

    setMode(mode) { this.mode = mode; }
    setWireframe(value) { this.wireframe = value; }
    setGhost(value) { this.ghost = value; }
    setDeformation(mode, custom = this.customDeform) { this.deformMode = mode; this.customDeform = custom; }
    setBoxes(fixture, load) {
        for (const box of this.boxes) this.gl.deleteBuffer(box.buffer);
        this.boxes = [
            {buffer: this.makeBuffer(boxLines(fixture)), color: cssColor("--fixture", [0.33, 0.84, 0.71])},
            {buffer: this.makeBuffer(boxLines(load)), color: cssColor("--load", [1, 0.54, 0.24])},
        ];
    }

    fieldRange(name) {
        if (!this.result || !this.result[`${name}Range`]) return null;
        return this.result[`${name}Range`].slice();
    }

    fit() {
        const min = this.bbox[0], max = this.bbox[1];
        this.target = [(min[0]+max[0])/2, (min[1]+max[1])/2, (min[2]+max[2])/2];
        const diagonal = Math.hypot(max[0]-min[0], max[1]-min[1], max[2]-min[2]);
        this.distance = diagonal > 1e-9 ? diagonal * 1.9 : 1;
    }

    eye() {
        const cp = Math.cos(this.pitch);
        // Matches the desktop Camera's 0.7 rad yaw, 0.5 rad pitch orientation.
        return [this.target[0] + this.distance*cp*Math.cos(this.yaw), this.target[1] + this.distance*cp*Math.sin(this.yaw), this.target[2] + this.distance*Math.sin(this.pitch)];
    }

    mvp() {
        const aspect = Math.max(1e-6, this.canvas.width / this.canvas.height);
        const near = Math.max(this.distance * 0.01, 1e-6);
        const far = Math.max(this.distance * 40, near + 1);
        return multiply(perspective(40 * Math.PI / 180, aspect, near, far), lookAt(this.eye(), this.target, [0, 0, 1]));
    }

    resize() {
        const dpr = Math.min(devicePixelRatio || 1, 2);
        const rect = this.canvas.getBoundingClientRect();
        const width = Math.max(1, Math.round(rect.width * dpr));
        const height = Math.max(1, Math.round(rect.height * dpr));
        if (this.canvas.width !== width || this.canvas.height !== height) {
            this.canvas.width = width;
            this.canvas.height = height;
        }
    }

    installInput() {
        const canvas = this.canvas;
        canvas.addEventListener("contextmenu", (event) => event.preventDefault());
        canvas.addEventListener("wheel", (event) => {
            event.preventDefault();
            this.distance *= Math.exp(event.deltaY * 0.001);
            this.distance = Math.max(this.distance, 1e-8);
        }, {passive: false});
        canvas.addEventListener("pointerdown", (event) => {
            canvas.setPointerCapture(event.pointerId);
            this.touches.set(event.pointerId, [event.clientX, event.clientY]);
            this.pointer = {id: event.pointerId, x: event.clientX, y: event.clientY, pan: event.button === 2 || event.shiftKey};
        });
        canvas.addEventListener("pointermove", (event) => {
            if (!this.touches.has(event.pointerId)) return;
            const previousTouches = [...this.touches.values()];
            this.touches.set(event.pointerId, [event.clientX, event.clientY]);
            const currentTouches = [...this.touches.values()];
            if (currentTouches.length === 2 && previousTouches.length === 2) {
                const oldDistance = Math.hypot(previousTouches[0][0]-previousTouches[1][0], previousTouches[0][1]-previousTouches[1][1]);
                const newDistance = Math.hypot(currentTouches[0][0]-currentTouches[1][0], currentTouches[0][1]-currentTouches[1][1]);
                if (oldDistance > 0 && newDistance > 0) this.distance *= oldDistance / newDistance;
                const oldCenter = [(previousTouches[0][0]+previousTouches[1][0])/2, (previousTouches[0][1]+previousTouches[1][1])/2];
                const newCenter = [(currentTouches[0][0]+currentTouches[1][0])/2, (currentTouches[0][1]+currentTouches[1][1])/2];
                this.pan(newCenter[0]-oldCenter[0], newCenter[1]-oldCenter[1]);
            } else if (this.pointer && this.pointer.id === event.pointerId) {
                const dx = event.clientX - this.pointer.x;
                const dy = event.clientY - this.pointer.y;
                if (this.pointer.pan) this.pan(dx, dy);
                else {
                    this.yaw -= dx * 0.008;
                    this.pitch = Math.max(-1.55, Math.min(1.55, this.pitch + dy * 0.008));
                }
                this.pointer.x = event.clientX;
                this.pointer.y = event.clientY;
            }
        });
        const release = (event) => { this.touches.delete(event.pointerId); if (this.pointer?.id === event.pointerId) this.pointer = null; };
        canvas.addEventListener("pointerup", release);
        canvas.addEventListener("pointercancel", release);
    }

    pan(dx, dy) {
        const eye = this.eye();
        const forward = normalize(sub(this.target, eye));
        const right = normalize(cross(forward, [0, 0, 1]));
        const up = normalize(cross(right, forward));
        const scale = 2 * this.distance * Math.tan(20 * Math.PI / 180) / Math.max(1, this.canvas.clientHeight);
        for (let i = 0; i < 3; ++i) this.target[i] += (-dx * right[i] + dy * up[i]) * scale;
    }

    bindAttribute(location, buffer, size, fallback = null) {
        const gl = this.gl;
        if (buffer) {
            gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
            gl.enableVertexAttribArray(location);
            gl.vertexAttribPointer(location, size, gl.FLOAT, false, 0, 0);
        } else {
            gl.disableVertexAttribArray(location);
            if (size === 1) gl.vertexAttrib1f(location, fallback ?? 0);
            else gl.vertexAttrib3f(location, ...(fallback || [0, 0, 0]));
        }
    }

    deformationScale() {
        if (!this.result?.displacement) return 0;
        if (this.deformMode === "1") return 1;
        if (this.deformMode === "custom") return Math.max(0, Number(this.customDeform) || 0);
        const diagonal = Math.hypot(...this.bbox[1].map((v, i) => v - this.bbox[0][i]));
        const max = this.result.displacementRange?.[1] || 0;
        return max > 0 ? 0.08 * diagonal / max : 1;
    }

    drawSurface(value, mode, alpha, deform, mvp) {
        const gl = this.gl;
        if (!value?.count) return;
        gl.useProgram(this.surfaceProgram);
        this.bindAttribute(0, value.positions, 3);
        this.bindAttribute(1, value.normals, 3, [0, 0, 1]);
        this.bindAttribute(2, value.displacement, 3, [0, 0, 0]);
        const field = mode === "von_mises" || mode === "displacement" || mode === "eta" ? mode : null;
        this.bindAttribute(3, field ? value.fieldBuffers[field] : null, 1, 0);
        const range = field ? value[`${field}Range`] : [0, 1];
        gl.uniformMatrix4fv(gl.getUniformLocation(this.surfaceProgram, "u_mvp"), false, mvp);
        gl.uniform1f(gl.getUniformLocation(this.surfaceProgram, "u_deform"), deform);
        gl.uniform1f(gl.getUniformLocation(this.surfaceProgram, "u_min"), range?.[0] || 0);
        gl.uniform1f(gl.getUniformLocation(this.surfaceProgram, "u_max"), range?.[1] || 1);
        gl.uniform1i(gl.getUniformLocation(this.surfaceProgram, "u_mode"), field ? 1 : 0);
        const base = mode === "mesh" ? [0.42, 0.58, 0.92] : cssColor("--technical", [0.33, 0.84, 0.71]);
        gl.uniform3fv(gl.getUniformLocation(this.surfaceProgram, "u_base"), base);
        gl.uniform1f(gl.getUniformLocation(this.surfaceProgram, "u_alpha"), alpha);
        gl.drawArrays(gl.TRIANGLES, 0, value.count);
    }

    drawLines(buffer, count, color, mvp) {
        if (!buffer || !count) return;
        const gl = this.gl;
        gl.useProgram(this.lineProgram);
        this.bindAttribute(0, buffer, 3);
        gl.uniformMatrix4fv(gl.getUniformLocation(this.lineProgram, "u_mvp"), false, mvp);
        gl.uniform4fv(gl.getUniformLocation(this.lineProgram, "u_color"), color);
        gl.drawArrays(gl.LINES, 0, count);
    }

    drawReveal(mvp) {
        const gl = this.gl;
        if (!this.mesh?.count) return;
        gl.useProgram(this.revealProgram);
        this.bindAttribute(0, this.mesh.positions, 3);
        this.bindAttribute(1, this.mesh.centroids, 3);
        this.bindAttribute(2, this.mesh.indices, 1);
        this.bindAttribute(3, this.mesh.colors, 3);
        gl.uniformMatrix4fv(gl.getUniformLocation(this.revealProgram, "u_mvp"), false, mvp);
        gl.uniform1f(gl.getUniformLocation(this.revealProgram, "u_reveal"), this.reveal);
        gl.uniform1f(gl.getUniformLocation(this.revealProgram, "u_shrink"), kRevealShrink);
        gl.uniform1f(gl.getUniformLocation(this.revealProgram, "u_arrival"), kArrivalBand);
        gl.drawArrays(gl.TRIANGLES, 0, this.mesh.count);
    }

    draw(time) {
        this.resize();
        if (!kReducedMotion.matches && this.reveal < 1) {
            this.reveal = Math.min(1, (time - this.revealStart) / (kRevealSeconds * 1000));
        }
        const gl = this.gl;
        gl.viewport(0, 0, this.canvas.width, this.canvas.height);
        const bg = cssColor("--bg-deep", [0.05, 0.06, 0.05]);
        gl.clearColor(bg[0], bg[1], bg[2], 1);
        gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
        gl.enable(gl.DEPTH_TEST);
        gl.enable(gl.BLEND);
        gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
        const mvp = this.mvp();
        if (this.mode === "cad") this.drawSurface(this.surface, "cad", 1, 0, mvp);
        else if (this.mode === "mesh" && this.mesh) this.drawReveal(mvp);
        else if (this.result) {
            const deform = this.deformationScale();
            if (this.ghost && deform !== 0) {
                gl.depthMask(false);
                this.drawSurface(this.result, "cad", 0.2, 0, mvp);
                gl.depthMask(true);
            }
            this.drawSurface(this.result, this.mode, 1, deform, mvp);
        }
        if (this.wireframe) {
            const edgeSource = this.mode === "mesh" && this.mesh ? this.mesh : (this.result || this.surface);
            this.drawLines(edgeSource?.edges, edgeSource?.edgeCount, [0.94, 0.92, 0.86, 0.42], mvp);
        }
        gl.disable(gl.DEPTH_TEST);
        for (const box of this.boxes) this.drawLines(box.buffer, 24, [...box.color, 0.95], mvp);
        requestAnimationFrame((next) => this.draw(next));
    }
}
