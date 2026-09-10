/*
 * OpenNeato no-go shape tools
 *
 * Adds rectangle/circle placement, moving and resizing to the existing
 * Home Assistant replay-card no-go editor without changing the robot wire
 * format. Shapes are flattened to the same arrays of {x,y} polyline points
 * already accepted by openneato/nogo_set.
 */

(() => {
    "use strict";

    const PATCH_VERSION = "1.0.0";
    const CIRCLE_SEGMENTS = 20;
    const MIN_SIZE_M = 0.18;
    const DEFAULT_BOX_M = 0.8;
    const DEFAULT_CIRCLE_RADIUS_M = 0.4;
    const HANDLE_HIT_M = 0.24;

    const round3 = (value) => Number(Number(value).toFixed(3));
    const clonePoint = (point) => ({ x: Number(point.x), y: Number(point.y) });
    const distance = (a, b) => Math.hypot(a.x - b.x, a.y - b.y);
    const closeEnough = (a, b, eps = 0.02) => Boolean(a && b) && distance(a, b) <= eps;

    function makeBox(cx, cy, width, height) {
        const hw = Math.max(MIN_SIZE_M, Math.abs(width)) / 2;
        const hh = Math.max(MIN_SIZE_M, Math.abs(height)) / 2;
        const points = [
            { x: cx - hw, y: cy - hh },
            { x: cx + hw, y: cy - hh },
            { x: cx + hw, y: cy + hh },
            { x: cx - hw, y: cy + hh },
        ].map((p) => ({ x: round3(p.x), y: round3(p.y) }));
        points.push(clonePoint(points[0]));
        return points;
    }

    function makeCircle(cx, cy, radius) {
        const r = Math.max(MIN_SIZE_M / 2, Math.abs(radius));
        const points = [];
        for (let i = 0; i <= CIRCLE_SEGMENTS; i++) {
            const angle = (Math.PI * 2 * i) / CIRCLE_SEGMENTS;
            points.push({
                x: round3(cx + Math.cos(angle) * r),
                y: round3(cy + Math.sin(angle) * r),
            });
        }
        points[points.length - 1] = clonePoint(points[0]);
        return points;
    }

    function lineBounds(line) {
        const usable = closeEnough(line?.[0], line?.[line.length - 1]) ? line.slice(0, -1) : line || [];
        if (!usable.length) return null;
        let minX = Infinity;
        let maxX = -Infinity;
        let minY = Infinity;
        let maxY = -Infinity;
        for (const p of usable) {
            minX = Math.min(minX, Number(p.x));
            maxX = Math.max(maxX, Number(p.x));
            minY = Math.min(minY, Number(p.y));
            maxY = Math.max(maxY, Number(p.y));
        }
        return {
            minX,
            maxX,
            minY,
            maxY,
            cx: (minX + maxX) / 2,
            cy: (minY + maxY) / 2,
            width: maxX - minX,
            height: maxY - minY,
        };
    }

    function inferKind(line) {
        if (!Array.isArray(line) || line.length < 5 || !closeEnough(line[0], line[line.length - 1])) return null;
        if (line.length === 5) return "box";
        if (line.length >= 13) {
            const b = lineBounds(line);
            if (!b || b.width <= 0 || b.height <= 0) return null;
            const ratio = b.width / b.height;
            if (ratio > 0.72 && ratio < 1.38) return "circle";
        }
        return null;
    }

    function shapeInfo(line, forcedKind = null) {
        const kind = forcedKind || inferKind(line);
        const b = lineBounds(line);
        if (!kind || !b) return null;
        if (kind === "circle") {
            return {
                kind,
                ...b,
                radius: (b.width + b.height) / 4,
                resize: { x: b.maxX, y: b.cy },
            };
        }
        return {
            kind,
            ...b,
            resize: { x: b.maxX, y: b.maxY },
        };
    }

    function setMode(card, mode) {
        if (!card._nogoEditing) return;
        card._nogoShapeMode = mode;
        if (mode !== "line") card._nogoCurrent = [];
        syncButtons(card);
        const messages = {
            line: "Tap points for a no-go line; use New line between barriers",
            box: "Drag on the map to place and size a no-go box",
            circle: "Drag on the map to place and size a circular no-go area",
            move: "Drag a box/circle to move it; drag its white handle to resize",
        };
        if (card._nogoNote) card._nogoNote.textContent = messages[mode] || messages.line;
        card._dirty = true;
        card._scheduleRender?.();
    }

    function syncButtons(card) {
        const editing = Boolean(card._nogoEditing);
        const mode = card._nogoShapeMode || "line";
        for (const [name, button] of Object.entries(card._nogoShapeButtons || {})) {
            button.hidden = !editing;
            button.classList.toggle("active", editing && name === mode);
            button.setAttribute("aria-pressed", editing && name === mode ? "true" : "false");
        }
        if (card._nogoNewBtn) card._nogoNewBtn.hidden = !editing || mode !== "line";
    }

    function eventToWorld(card, event) {
        if (!card._lastProjection || !card._lastViewMatrix || !card._canvas) return null;
        const rect = card._canvas.getBoundingClientRect();
        const dpr = card._lastDpr || 1;
        const device = new DOMPoint((event.clientX - rect.left) * dpr, (event.clientY - rect.top) * dpr);
        const projected = card._lastViewMatrix.inverse().transformPoint(device);
        return {
            x: round3(card._lastProjection.fromX(projected.x)),
            y: round3(card._lastProjection.fromY(projected.y)),
        };
    }

    function findShape(card, point) {
        let best = null;
        for (let i = (card._nogoDraft || []).length - 1; i >= 0; i--) {
            const line = card._nogoDraft[i];
            const forcedKind = card._nogoShapeKinds?.get(i) || null;
            const info = shapeInfo(line, forcedKind);
            if (!info) continue;
            const handleDistance = distance(point, info.resize);
            if (handleDistance <= HANDLE_HIT_M) return { index: i, info, hit: "resize" };

            let inside = false;
            if (info.kind === "circle") {
                inside = distance(point, { x: info.cx, y: info.cy }) <= info.radius + HANDLE_HIT_M * 0.5;
            } else {
                inside =
                    point.x >= info.minX - HANDLE_HIT_M * 0.25 &&
                    point.x <= info.maxX + HANDLE_HIT_M * 0.25 &&
                    point.y >= info.minY - HANDLE_HIT_M * 0.25 &&
                    point.y <= info.maxY + HANDLE_HIT_M * 0.25;
            }
            if (inside && !best) best = { index: i, info, hit: "move" };
        }
        return best;
    }

    function installCanvasHandlers(card) {
        const canvas = card._canvas;
        if (!canvas || canvas.__openNeatoShapeHandlers) return;
        canvas.__openNeatoShapeHandlers = true;

        const swallow = (event) => {
            if (!card._nogoEditing || (card._nogoShapeMode || "line") === "line") return false;
            event.preventDefault();
            event.stopImmediatePropagation();
            return true;
        };

        canvas.addEventListener(
            "click",
            (event) => {
                swallow(event);
            },
            true,
        );

        canvas.addEventListener(
            "pointerdown",
            (event) => {
                if (!swallow(event)) return;
                const point = eventToWorld(card, event);
                if (!point) return;
                const mode = card._nogoShapeMode || "line";
                canvas.setPointerCapture?.(event.pointerId);

                if (mode === "box" || mode === "circle") {
                    const index = card._nogoDraft.length;
                    const line =
                        mode === "box"
                            ? makeBox(point.x, point.y, DEFAULT_BOX_M, DEFAULT_BOX_M)
                            : makeCircle(point.x, point.y, DEFAULT_CIRCLE_RADIUS_M);
                    card._nogoDraft.push(line);
                    card._nogoShapeKinds.set(index, mode);
                    card._nogoShapeSelected = index;
                    card._nogoShapeGesture = {
                        type: "create",
                        kind: mode,
                        index,
                        center: point,
                        start: point,
                        moved: false,
                    };
                } else if (mode === "move") {
                    const hit = findShape(card, point);
                    if (!hit) {
                        card._nogoShapeSelected = null;
                        card._nogoShapeGesture = null;
                        card._nogoNote.textContent = "Tap a box or circle first";
                    } else {
                        card._nogoShapeSelected = hit.index;
                        card._nogoShapeKinds.set(hit.index, hit.info.kind);
                        card._nogoShapeGesture = {
                            type: hit.hit,
                            kind: hit.info.kind,
                            index: hit.index,
                            start: point,
                            center: { x: hit.info.cx, y: hit.info.cy },
                            original: card._nogoDraft[hit.index].map(clonePoint),
                            originalInfo: hit.info,
                        };
                    }
                }
                card._dirty = true;
                card._scheduleRender?.();
            },
            true,
        );

        canvas.addEventListener(
            "pointermove",
            (event) => {
                if (!card._nogoEditing || !card._nogoShapeGesture) return;
                if (!swallow(event)) return;
                const point = eventToWorld(card, event);
                if (!point) return;
                const g = card._nogoShapeGesture;
                if (!Number.isInteger(g.index) || !card._nogoDraft[g.index]) return;

                if (g.type === "create") {
                    const dx = point.x - g.center.x;
                    const dy = point.y - g.center.y;
                    g.moved = g.moved || Math.hypot(dx, dy) > 0.04;
                    if (g.kind === "box") {
                        card._nogoDraft[g.index] = makeBox(g.center.x, g.center.y, Math.abs(dx) * 2, Math.abs(dy) * 2);
                    } else {
                        card._nogoDraft[g.index] = makeCircle(g.center.x, g.center.y, Math.hypot(dx, dy));
                    }
                } else if (g.type === "move") {
                    const dx = point.x - g.start.x;
                    const dy = point.y - g.start.y;
                    card._nogoDraft[g.index] = g.original.map((p) => ({ x: round3(p.x + dx), y: round3(p.y + dy) }));
                } else if (g.type === "resize") {
                    if (g.kind === "circle") {
                        card._nogoDraft[g.index] = makeCircle(g.center.x, g.center.y, distance(point, g.center));
                    } else {
                        card._nogoDraft[g.index] = makeBox(
                            g.center.x,
                            g.center.y,
                            Math.abs(point.x - g.center.x) * 2,
                            Math.abs(point.y - g.center.y) * 2,
                        );
                    }
                }
                card._dirty = true;
                card._scheduleRender?.();
            },
            true,
        );

        const finishPointer = (event) => {
            if (!card._nogoEditing || !card._nogoShapeGesture) return;
            if (!swallow(event)) return;
            const g = card._nogoShapeGesture;
            card._nogoShapeGesture = null;
            canvas.releasePointerCapture?.(event.pointerId);
            if (g.type === "create") {
                card._nogoNote.textContent = `${g.kind === "circle" ? "Circle" : "Box"} added — drag it or its white handle to adjust`;
                setMode(card, "move");
            }
            card._dirty = true;
            card._scheduleRender?.();
        };
        canvas.addEventListener("pointerup", finishPointer, true);
        canvas.addEventListener("pointercancel", finishPointer, true);
    }

    function addToolbar(card) {
        const root = card.shadowRoot;
        if (!root || root.querySelector(".nogo-shape-line")) return;
        const controls = root.querySelector(".nogo-controls");
        const newLine = root.querySelector(".nogo-new");
        if (!controls || !newLine) return;

        const defs = [
            ["line", "Line", "Draw a free no-go line"],
            ["box", "▢ Box", "Draw a rectangular no-go area"],
            ["circle", "⭕ Circle", "Draw a circular no-go area"],
            ["move", "✋ Move", "Move or resize a box/circle"],
        ];
        card._nogoShapeButtons = {};
        for (const [mode, label, title] of defs) {
            const button = document.createElement("button");
            button.className = `nogo-action nogo-shape-${mode}`;
            button.textContent = label;
            button.title = title;
            button.hidden = true;
            button.type = "button";
            button.addEventListener("click", (event) => {
                event.preventDefault();
                event.stopPropagation();
                if (mode !== "line" && card._nogoCurrent?.length) {
                    if (card._nogoCurrent.length >= 2) card._nogoDraft.push(card._nogoCurrent);
                    card._nogoCurrent = [];
                }
                setMode(card, mode);
            });
            controls.insertBefore(button, newLine);
            card._nogoShapeButtons[mode] = button;
        }
        card._nogoNewBtn = newLine;
        syncButtons(card);
        installCanvasHandlers(card);
    }

    customElements.whenDefined("openneato-replay-card").then(() => {
        const Card = customElements.get("openneato-replay-card");
        if (!Card || Card.prototype.__openNeatoShapePatch) return;
        const proto = Card.prototype;
        proto.__openNeatoShapePatch = PATCH_VERSION;

        const originalBuildDom = proto._buildDom;
        proto._buildDom = function (...args) {
            const result = originalBuildDom.apply(this, args);
            this._nogoShapeMode = this._nogoShapeMode || "line";
            this._nogoShapeKinds = this._nogoShapeKinds || new Map();
            this._nogoShapeSelected = null;
            this._nogoShapeGesture = null;
            addToolbar(this);
            return result;
        };

        const originalBegin = proto._beginNoGoEdit;
        proto._beginNoGoEdit = function (...args) {
            const result = originalBegin.apply(this, args);
            this._nogoShapeMode = "line";
            this._nogoShapeKinds = new Map();
            for (let i = 0; i < (this._nogoDraft || []).length; i++) {
                const kind = inferKind(this._nogoDraft[i]);
                if (kind) this._nogoShapeKinds.set(i, kind);
            }
            this._nogoShapeSelected = null;
            this._nogoShapeGesture = null;
            syncButtons(this);
            return result;
        };

        const originalSetUi = proto._setNoGoEditUi;
        proto._setNoGoEditUi = function (...args) {
            const result = originalSetUi.apply(this, args);
            syncButtons(this);
            return result;
        };

        const originalUndo = proto._undoNoGoPoint;
        proto._undoNoGoPoint = function (...args) {
            const result = originalUndo.apply(this, args);
            if (this._nogoShapeSelected != null && this._nogoShapeSelected >= (this._nogoDraft || []).length) {
                this._nogoShapeSelected = null;
            }
            this._nogoShapeKinds = new Map(
                [...(this._nogoShapeKinds || new Map()).entries()].filter(([index]) => index < (this._nogoDraft || []).length),
            );
            return result;
        };

        const originalFinish = proto._finishNoGoEdit;
        proto._finishNoGoEdit = function (...args) {
            const result = originalFinish.apply(this, args);
            this._nogoShapeMode = "line";
            this._nogoShapeSelected = null;
            this._nogoShapeGesture = null;
            this._nogoShapeKinds = new Map();
            syncButtons(this);
            return result;
        };

        const originalDraw = proto._drawNoGoLines;
        proto._drawNoGoLines = function (ctx, proj, ...rest) {
            const result = originalDraw.call(this, ctx, proj, ...rest);
            if (!this._nogoEditing || this._nogoShapeSelected == null) return result;
            const line = this._nogoDraft?.[this._nogoShapeSelected];
            const forcedKind = this._nogoShapeKinds?.get(this._nogoShapeSelected) || null;
            const info = shapeInfo(line, forcedKind);
            if (!info) return result;

            const cx = proj.toX(info.cx);
            const cy = proj.toY(info.cy);
            const hx = proj.toX(info.resize.x);
            const hy = proj.toY(info.resize.y);
            const scale = Math.max(0.6, Number(this._tf?.zoom) || 1);
            const handle = 6 / scale;

            ctx.save();
            ctx.setLineDash([]);
            ctx.fillStyle = "rgba(255,255,255,0.95)";
            ctx.strokeStyle = "rgba(255,59,48,1)";
            ctx.lineWidth = Math.max(1.5, 2 / scale);
            ctx.beginPath();
            ctx.arc(cx, cy, handle * 0.65, 0, Math.PI * 2);
            ctx.fill();
            ctx.stroke();
            ctx.beginPath();
            ctx.arc(hx, hy, handle, 0, Math.PI * 2);
            ctx.fill();
            ctx.stroke();
            ctx.restore();
            return result;
        };

        console.info(`[OpenNeato] no-go shape tools ${PATCH_VERSION} loaded`);
    });
})();
