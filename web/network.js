const kMaxConnections = 180;
const kMaxNodeRadius = 8;
const kRdBuReversed = [
    [5, 48, 97], [33, 102, 172], [67, 147, 195], [146, 197, 222],
    [209, 229, 240], [247, 247, 247], [253, 219, 199], [244, 165, 130],
    [214, 96, 77], [178, 24, 43], [103, 0, 31],
].map((color) => color.map((value) => value / 255));
const kReducedMotion = matchMedia("(prefers-reduced-motion: reduce)");

function colorWithAlpha(rgb, alpha = 1) {
    return `rgba(${Math.round(rgb[0] * 255)},${Math.round(rgb[1] * 255)},${Math.round(rgb[2] * 255)},${alpha})`;
}

// Exact ColorBrewer RdBu 11-class controls, reversed: negative blue, positive
// red. Interpolation mirrors the desktop's signed_colormap implementation.
export function signedColor(value) {
    const t = Math.max(-1, Math.min(1, Number(value) || 0));
    const u = 0.5 * (t + 1) * (kRdBuReversed.length - 1);
    const lo = Math.min(Math.floor(u), kRdBuReversed.length - 1);
    const hi = Math.min(lo + 1, kRdBuReversed.length - 1);
    const f = u - lo;
    return kRdBuReversed[lo].map((v, i) => v + f * (kRdBuReversed[hi][i] - v));
}

function token(name, fallback) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim() || fallback;
}

function roundedRect(ctx, x, y, width, height, radius) {
    const r = Math.min(radius, width / 2, height / 2);
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + width, y, x + width, y + height, r);
    ctx.arcTo(x + width, y + height, x, y + height, r);
    ctx.arcTo(x, y + height, x, y, r);
    ctx.arcTo(x, y, x + width, y, r);
    ctx.closePath();
}

function activationArray(frame, name) {
    return Array.isArray(frame?.[name]) ? frame[name] : [];
}

export class NetworkPanel {
    constructor(canvas) {
        this.canvas = canvas;
        this.ctx = canvas.getContext("2d");
        this.data = null;
        this.frameIndex = 0;
        this.frameStarted = 0;
        this.currentLinks = [];
        this.retiring = false;
        this.hideTimer = null;
        this.observer = new ResizeObserver(() => this.resize());
        this.observer.observe(canvas);
        requestAnimationFrame((time) => this.draw(time));
    }

    setData(data) {
        this.data = data;
        const recommended = data.frames?.findIndex((frame) => frame.recommended) ?? -1;
        this.frameIndex = kReducedMotion.matches && recommended >= 0 ? recommended : 0;
        this.frameStarted = performance.now();
        this.retiring = false;
        clearTimeout(this.hideTimer);
        this.canvas.hidden = false;
        this.canvas.classList.remove("retiring");
        this.updateLinks();
        this.resize();
    }

    setProgress(progress) {
        if (!this.data || this.retiring) return;
        if (progress.phase === "mesh" && Number(progress.phase_frac) >= 0.45) this.retire();
    }

    setMeshStage(index) {
        if (this.data && !this.retiring && Number(index) >= 5) this.retire();
    }

    retire() {
        this.retiring = true;
        this.canvas.classList.add("retiring");
        this.hideTimer = setTimeout(() => { if (this.retiring) this.canvas.hidden = true; }, 900);
    }

    reset() {
        this.data = null;
        this.retiring = false;
        clearTimeout(this.hideTimer);
        this.canvas.hidden = true;
        this.canvas.classList.remove("retiring");
    }

    resize() {
        if (this.canvas.hidden) return;
        const dpr = Math.min(devicePixelRatio || 1, 2);
        const rect = this.canvas.getBoundingClientRect();
        const width = Math.max(1, Math.round(rect.width * dpr));
        const height = Math.max(1, Math.round(rect.height * dpr));
        if (this.canvas.width !== width || this.canvas.height !== height) {
            this.canvas.width = width;
            this.canvas.height = height;
        }
    }

    updateLinks() {
        const frame = this.data?.frames?.[this.frameIndex];
        if (!frame) { this.currentLinks = []; return; }
        const links = [];
        for (const edge of this.data.edges || []) {
            const input = activationArray(frame, edge.from);
            const rows = Number(edge.rows) || 0;
            const cols = Number(edge.cols) || 0;
            if (input.length < cols || !Array.isArray(edge.weights)) continue;
            const availableRows = Math.min(rows, activationArray(frame, edge.to).length);
            for (let row = 0; row < availableRows; ++row) {
                const offset = row * cols;
                for (let col = 0; col < cols; ++col) {
                    const contribution = (Number(edge.weights[offset + col]) || 0) * (Number(input[col]) || 0);
                    links.push({from: edge.from, to: edge.to, i: col, j: row, contribution, magnitude: Math.abs(contribution)});
                }
            }
        }
        links.sort((a, b) => b.magnitude - a.magnitude);
        this.currentLinks = links.slice(0, kMaxConnections)
            .sort((a, b) => a.magnitude - b.magnitude);
    }

    layout(width, height, frame) {
        const names = ["input", "fc1", "fc2", "heads"];
        // 48, not 40: each lane's label is drawn 13px above its own baseline,
        // so the first lane needs clearance from the panel title at y=16.
        const top = 48;
        const candidateHeight = 34;
        const bottom = height - candidateHeight - 18;
        const lanes = {};
        names.forEach((name, laneIndex) => {
            const values = activationArray(frame, name);
            const y = top + laneIndex * ((bottom - top) / (names.length - 1));
            const usable = width - 56;
            lanes[name] = values.map((_, index) => [28 + usable * (index + 0.5) / Math.max(1, values.length), y]);
        });
        return {lanes, candidateY: height - 20};
    }

    drawLinks(ctx, layout) {
        const scale = Math.max(Number(this.data.scale?.contribution) || 0, Number.EPSILON);
        ctx.lineCap = "round";
        for (const link of this.currentLinks) {
            const from = layout.lanes[link.from]?.[link.i];
            const to = layout.lanes[link.to]?.[link.j];
            if (!from || !to) continue;
            const strength = Math.min(1, Math.abs(link.contribution) / scale);
            const sign = link.contribution < 0 ? -1 : 1;
            const signedRamp = sign * (0.34 + 0.66 * strength);
            ctx.strokeStyle = colorWithAlpha(
                signedColor(signedRamp),
                0.06 + 0.84 * strength,
            );
            ctx.lineWidth = 0.55 + 1.55 * strength;
            ctx.beginPath();
            ctx.moveTo(from[0], from[1]);
            ctx.lineTo(to[0], to[1]);
            ctx.stroke();
        }
    }

    drawNodes(ctx, layout, frame) {
        const names = ["input", "fc1", "fc2", "heads"];
        ctx.font = `10px ${token("--mono", "monospace")}`;
        ctx.textBaseline = "middle";
        for (const name of names) {
            const values = activationArray(frame, name);
            const scale = Math.max(Number(this.data.scale?.[name]) || 0, Number.EPSILON);
            const points = layout.lanes[name] || [];
            const spacing = points.length > 1 ? points[1][0] - points[0][0] : kMaxNodeRadius * 2;
            const radiusMax = Math.max(2.6, Math.min(kMaxNodeRadius, 0.46 * spacing));
            // Label sits ABOVE its lane, not beside it. At 10px mono "INPUT" is
            // ~30px wide but the first node sits at x=28, so a left-aligned
            // label on the lane's own baseline was overrun by the lane itself.
            ctx.fillStyle = token("--faint", "#85857f");
            ctx.fillText(name.toUpperCase(), 10, (points[0]?.[1] ?? 0) - 13);
            values.forEach((raw, index) => {
                const normalized = Math.max(-1, Math.min(1, (Number(raw) || 0) / scale));
                const magnitude = Math.min(1, Math.abs(normalized));
                const radius = 1.8 + (radiusMax - 1.8) * magnitude;
                const point = points[index];
                if (!point) return;
                ctx.fillStyle = colorWithAlpha(signedColor(normalized), 0.45 + 0.55 * magnitude);
                ctx.beginPath();
                ctx.arc(point[0], point[1], radius, 0, Math.PI * 2);
                ctx.fill();
                if (name === "heads" && index === Number(this.data.winner)) {
                    ctx.save();
                    ctx.strokeStyle = token("--accent-hi", "#ffad73");
                    ctx.shadowColor = token("--accent", "#ff8a3d");
                    ctx.shadowBlur = 10;
                    ctx.lineWidth = 1.8;
                    ctx.beginPath();
                    ctx.arc(point[0], point[1], radius + 4, 0, Math.PI * 2);
                    ctx.stroke();
                    ctx.restore();
                }
            });
        }
    }

    drawCandidates(ctx, width, y) {
        const frames = this.data.frames || [];
        if (!frames.length) return;
        const scores = frames.map((frame) => Number(frame.score)).filter(Number.isFinite);
        const low = Math.min(...scores);
        const high = Math.max(...scores);
        const start = 28;
        const usable = width - 56;
        ctx.strokeStyle = token("--line-hi", "#5d6258");
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(start, y);
        ctx.lineTo(start + usable, y);
        ctx.stroke();
        frames.forEach((frame, index) => {
            const x = start + usable * (index + 0.5) / frames.length;
            const score = Number(frame.score);
            const normalized = high > low && Number.isFinite(score) ? (score - low) / (high - low) : 0.5;
            const radius = 2.5 + 3.5 * normalized;
            ctx.fillStyle = frame.gate_pass ? token("--technical", "#53d6b5") : token("--faint", "#85857f");
            ctx.beginPath();
            ctx.arc(x, y, radius, 0, Math.PI * 2);
            ctx.fill();
            if (frame.recommended) {
                ctx.strokeStyle = token("--accent-hi", "#ffad73");
                ctx.lineWidth = 2;
                ctx.beginPath();
                ctx.arc(x, y, radius + 4, 0, Math.PI * 2);
                ctx.stroke();
            }
        });
        ctx.fillStyle = token("--faint", "#85857f");
        ctx.font = `9px ${token("--mono", "monospace")}`;
        ctx.fillText("CANDIDATES", 10, y - 12);
    }

    draw(time) {
        this.resize();
        const data = this.data;
        if (data && !this.canvas.hidden) {
            const frames = data.frames || [];
            if (!kReducedMotion.matches && !this.retiring && frames.length > 1 && time - this.frameStarted >= 850) {
                this.frameIndex = (this.frameIndex + 1) % frames.length;
                this.frameStarted = time;
                this.updateLinks();
            }
            const frame = frames[this.frameIndex];
            if (frame) {
                const dpr = this.canvas.width / Math.max(1, this.canvas.clientWidth);
                const width = this.canvas.width / dpr;
                const height = this.canvas.height / dpr;
                const ctx = this.ctx;
                ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
                ctx.clearRect(0, 0, width, height);
                roundedRect(ctx, 0.5, 0.5, width - 1, height - 1, 10);
                ctx.fillStyle = "rgba(12,14,12,.86)";
                ctx.fill();
                ctx.strokeStyle = token("--line-hi", "#5d6258");
                ctx.lineWidth = 1;
                ctx.stroke();
                const layout = this.layout(width, height, frame);
                this.drawLinks(ctx, layout);
                this.drawNodes(ctx, layout, frame);
                this.drawCandidates(ctx, width, layout.candidateY);
                ctx.fillStyle = token("--technical", "#53d6b5");
                ctx.font = `10px ${token("--mono", "monospace")}`;
                ctx.fillText("ADVISOR / LIVE ACTIVATIONS", 12, 16);
            }
        }
        requestAnimationFrame((next) => this.draw(next));
    }
}
