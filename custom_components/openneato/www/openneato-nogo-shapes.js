/* OpenNeato no-go shape tools: reliable toolbar + move/resize + card styling. */
(() => {
  "use strict";

  const PATCH_VERSION = "1.1.0";
  const CIRCLE_SEGMENTS = 20;
  const MIN_SIZE_M = 0.18;
  const DEFAULT_BOX_M = 0.8;
  const DEFAULT_CIRCLE_RADIUS_M = 0.4;
  const HANDLE_HIT_M = 0.24;

  const round3 = (v) => Number(Number(v).toFixed(3));
  const clonePoint = (p) => ({ x: Number(p.x), y: Number(p.y) });
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
      const a = (Math.PI * 2 * i) / CIRCLE_SEGMENTS;
      points.push({ x: round3(cx + Math.cos(a) * r), y: round3(cy + Math.sin(a) * r) });
    }
    points[points.length - 1] = clonePoint(points[0]);
    return points;
  }

  function lineBounds(line) {
    if (!Array.isArray(line) || !line.length) return null;
    const pts = closeEnough(line[0], line[line.length - 1]) ? line.slice(0, -1) : line;
    if (!pts.length) return null;
    let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
    for (const p of pts) {
      minX = Math.min(minX, Number(p.x)); maxX = Math.max(maxX, Number(p.x));
      minY = Math.min(minY, Number(p.y)); maxY = Math.max(maxY, Number(p.y));
    }
    return { minX, maxX, minY, maxY, cx: (minX + maxX) / 2, cy: (minY + maxY) / 2, width: maxX - minX, height: maxY - minY };
  }

  function inferKind(line) {
    if (!Array.isArray(line) || line.length < 5 || !closeEnough(line[0], line[line.length - 1])) return null;
    if (line.length === 5) return "box";
    if (line.length >= 13) {
      const b = lineBounds(line);
      if (b && b.width > 0 && b.height > 0) {
        const ratio = b.width / b.height;
        if (ratio > 0.72 && ratio < 1.38) return "circle";
      }
    }
    return null;
  }

  function shapeInfo(line, forcedKind = null) {
    const kind = forcedKind || inferKind(line);
    const b = lineBounds(line);
    if (!kind || !b) return null;
    if (kind === "circle") return { kind, ...b, radius: (b.width + b.height) / 4, resize: { x: b.maxX, y: b.cy } };
    return { kind, ...b, resize: { x: b.maxX, y: b.maxY } };
  }

  function ensureState(card) {
    card._nogoShapeMode = card._nogoShapeMode || "line";
    card._nogoShapeKinds = card._nogoShapeKinds || new Map();
    if (card._nogoShapeSelected === undefined) card._nogoShapeSelected = null;
    if (card._nogoShapeGesture === undefined) card._nogoShapeGesture = null;
  }

  function syncButtons(card) {
    ensureState(card);
    const editing = Boolean(card._nogoEditing);
    const mode = card._nogoShapeMode || "line";
    for (const [name, button] of Object.entries(card._nogoShapeButtons || {})) {
      button.hidden = !editing;
      button.classList.toggle("active", editing && name === mode);
      button.setAttribute("aria-pressed", editing && name === mode ? "true" : "false");
    }
    if (card._nogoNewBtn) card._nogoNewBtn.hidden = !editing || mode !== "line";
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
      move: "Drag a box/circle to move it; drag the white handle to resize",
    };
    if (card._nogoNote) card._nogoNote.textContent = messages[mode] || messages.line;
    card._dirty = true;
    card._scheduleRender?.();
  }

  function eventToWorld(card, event) {
    if (!card._lastProjection || !card._lastViewMatrix || !card._canvas) return null;
    const rect = card._canvas.getBoundingClientRect();
    const dpr = card._lastDpr || 1;
    const device = new DOMPoint((event.clientX - rect.left) * dpr, (event.clientY - rect.top) * dpr);
    const projected = card._lastViewMatrix.inverse().transformPoint(device);
    return { x: round3(card._lastProjection.fromX(projected.x)), y: round3(card._lastProjection.fromY(projected.y)) };
  }

  function findShape(card, point) {
    for (let i = (card._nogoDraft || []).length - 1; i >= 0; i--) {
      const info = shapeInfo(card._nogoDraft[i], card._nogoShapeKinds?.get(i) || null);
      if (!info) continue;
      if (distance(point, info.resize) <= HANDLE_HIT_M) return { index: i, info, hit: "resize" };
      const inside = info.kind === "circle"
        ? distance(point, { x: info.cx, y: info.cy }) <= info.radius + HANDLE_HIT_M * 0.5
        : point.x >= info.minX - 0.06 && point.x <= info.maxX + 0.06 && point.y >= info.minY - 0.06 && point.y <= info.maxY + 0.06;
      if (inside) return { index: i, info, hit: "move" };
    }
    return null;
  }

  function installCanvasHandlers(card) {
    const canvas = card._canvas;
    if (!canvas || canvas.__openNeatoShapeHandlersV11) return;
    canvas.__openNeatoShapeHandlersV11 = true;

    const swallow = (event) => {
      if (!card._nogoEditing || (card._nogoShapeMode || "line") === "line") return false;
      event.preventDefault();
      event.stopImmediatePropagation();
      return true;
    };

    canvas.addEventListener("click", (e) => swallow(e), true);
    canvas.addEventListener("pointerdown", (event) => {
      if (!swallow(event)) return;
      const point = eventToWorld(card, event);
      if (!point) return;
      ensureState(card);
      const mode = card._nogoShapeMode || "line";
      canvas.setPointerCapture?.(event.pointerId);

      if (mode === "box" || mode === "circle") {
        const index = card._nogoDraft.length;
        card._nogoDraft.push(mode === "box" ? makeBox(point.x, point.y, DEFAULT_BOX_M, DEFAULT_BOX_M) : makeCircle(point.x, point.y, DEFAULT_CIRCLE_RADIUS_M));
        card._nogoShapeKinds.set(index, mode);
        card._nogoShapeSelected = index;
        card._nogoShapeGesture = { type: "create", kind: mode, index, center: point };
      } else if (mode === "move") {
        const hit = findShape(card, point);
        if (!hit) {
          card._nogoShapeSelected = null;
          card._nogoShapeGesture = null;
          if (card._nogoNote) card._nogoNote.textContent = "Tap a box or circle first";
        } else {
          card._nogoShapeSelected = hit.index;
          card._nogoShapeKinds.set(hit.index, hit.info.kind);
          card._nogoShapeGesture = {
            type: hit.hit, kind: hit.info.kind, index: hit.index, start: point,
            center: { x: hit.info.cx, y: hit.info.cy }, original: card._nogoDraft[hit.index].map(clonePoint),
          };
        }
      }
      card._dirty = true;
      card._scheduleRender?.();
    }, true);

    canvas.addEventListener("pointermove", (event) => {
      if (!card._nogoEditing || !card._nogoShapeGesture || !swallow(event)) return;
      const point = eventToWorld(card, event);
      if (!point) return;
      const g = card._nogoShapeGesture;
      if (!Number.isInteger(g.index) || !card._nogoDraft[g.index]) return;
      if (g.type === "create") {
        const dx = point.x - g.center.x, dy = point.y - g.center.y;
        card._nogoDraft[g.index] = g.kind === "box"
          ? makeBox(g.center.x, g.center.y, Math.abs(dx) * 2, Math.abs(dy) * 2)
          : makeCircle(g.center.x, g.center.y, Math.hypot(dx, dy));
      } else if (g.type === "move") {
        const dx = point.x - g.start.x, dy = point.y - g.start.y;
        card._nogoDraft[g.index] = g.original.map((p) => ({ x: round3(p.x + dx), y: round3(p.y + dy) }));
      } else if (g.type === "resize") {
        card._nogoDraft[g.index] = g.kind === "circle"
          ? makeCircle(g.center.x, g.center.y, distance(point, g.center))
          : makeBox(g.center.x, g.center.y, Math.abs(point.x - g.center.x) * 2, Math.abs(point.y - g.center.y) * 2);
      }
      card._dirty = true;
      card._scheduleRender?.();
    }, true);

    const finish = (event) => {
      if (!card._nogoEditing || !card._nogoShapeGesture || !swallow(event)) return;
      const g = card._nogoShapeGesture;
      card._nogoShapeGesture = null;
      if (canvas.hasPointerCapture?.(event.pointerId)) canvas.releasePointerCapture(event.pointerId);
      if (g.type === "create") {
        if (card._nogoNote) card._nogoNote.textContent = `${g.kind === "circle" ? "Circle" : "Box"} added — drag it or its white handle to adjust`;
        setMode(card, "move");
      }
      card._dirty = true;
      card._scheduleRender?.();
    };
    canvas.addEventListener("pointerup", finish, true);
    canvas.addEventListener("pointercancel", finish, true);
  }

  function styleNoGoPanel(card) {
    const root = card.shadowRoot;
    if (!root || root.querySelector("style[data-openneato-nogo-panel]")) return;
    const style = document.createElement("style");
    style.dataset.openneatoNogoPanel = "1";
    style.textContent = `
      .nogo-controls {
        margin: 0 12px 12px !important;
        padding: 10px 12px !important;
        min-height: 42px !important;
        border: 1px solid var(--divider-color) !important;
        border-radius: 14px !important;
        background: var(--ha-card-background, var(--card-background-color)) !important;
        box-shadow: var(--ha-card-box-shadow, none);
      }
      .nogo-controls::before {
        content: "No-go zones";
        flex: 0 0 100%;
        font-size: .95rem;
        font-weight: 600;
        color: var(--primary-text-color);
        margin-bottom: 2px;
      }
      button.nogo-shape-line.active, button.nogo-shape-box.active,
      button.nogo-shape-circle.active, button.nogo-shape-move.active {
        border-color: var(--primary-color) !important;
        color: var(--primary-color) !important;
        background: color-mix(in srgb, var(--primary-color) 12%, transparent) !important;
      }
      .nogo-note { flex: 1 1 180px; }
    `;
    root.appendChild(style);
  }

  function addToolbar(card) {
    ensureState(card);
    const root = card.shadowRoot;
    if (!root) return;
    styleNoGoPanel(card);
    installCanvasHandlers(card);
    if (root.querySelector(".nogo-shape-line")) { syncButtons(card); return; }
    const controls = root.querySelector(".nogo-controls");
    const newLine = root.querySelector(".nogo-new");
    if (!controls || !newLine) return;
    card._nogoShapeButtons = {};
    const defs = [
      ["line", "Line", "Draw a free no-go line"],
      ["box", "▢ Box", "Draw a rectangular no-go area"],
      ["circle", "⭕ Circle", "Draw a circular no-go area"],
      ["move", "✋ Move", "Move or resize a box/circle"],
    ];
    for (const [mode, label, title] of defs) {
      const button = document.createElement("button");
      button.className = `nogo-action nogo-shape-${mode}`;
      button.textContent = label;
      button.title = title;
      button.hidden = true;
      button.type = "button";
      button.addEventListener("click", (event) => {
        event.preventDefault(); event.stopPropagation();
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
  }

  function visitOpenShadowRoots(root, callback) {
    if (!root) return;
    const nodes = root.querySelectorAll ? root.querySelectorAll("*") : [];
    for (const node of nodes) {
      if (node.localName === "openneato-replay-card") callback(node);
      if (node.shadowRoot) visitOpenShadowRoots(node.shadowRoot, callback);
    }
  }

  function patchExistingCards() {
    if (document.documentElement?.localName === "openneato-replay-card") addToolbar(document.documentElement);
    visitOpenShadowRoots(document, addToolbar);
  }

  customElements.whenDefined("openneato-replay-card").then(() => {
    const Card = customElements.get("openneato-replay-card");
    if (!Card) return;
    const proto = Card.prototype;

    if (proto.__openNeatoShapePatch !== PATCH_VERSION) {
      proto.__openNeatoShapePatch = PATCH_VERSION;

      const originalBuildDom = proto._buildDom;
      proto._buildDom = function (...args) {
        const result = originalBuildDom.apply(this, args);
        addToolbar(this);
        return result;
      };

      const originalBegin = proto._beginNoGoEdit;
      proto._beginNoGoEdit = function (...args) {
        const result = originalBegin.apply(this, args);
        ensureState(this);
        this._nogoShapeMode = "line";
        this._nogoShapeKinds = new Map();
        for (let i = 0; i < (this._nogoDraft || []).length; i++) {
          const kind = inferKind(this._nogoDraft[i]);
          if (kind) this._nogoShapeKinds.set(i, kind);
        }
        this._nogoShapeSelected = null;
        this._nogoShapeGesture = null;
        addToolbar(this);
        syncButtons(this);
        return result;
      };

      const originalSetUi = proto._setNoGoEditUi;
      proto._setNoGoEditUi = function (...args) {
        const result = originalSetUi.apply(this, args);
        addToolbar(this);
        syncButtons(this);
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
        const info = shapeInfo(line, this._nogoShapeKinds?.get(this._nogoShapeSelected) || null);
        if (!info) return result;
        const cx = proj.toX(info.cx), cy = proj.toY(info.cy), hx = proj.toX(info.resize.x), hy = proj.toY(info.resize.y);
        const scale = Math.max(0.6, Number(this._tf?.zoom) || 1), handle = 6 / scale;
        ctx.save(); ctx.setLineDash([]); ctx.fillStyle = "rgba(255,255,255,.95)"; ctx.strokeStyle = "rgba(255,59,48,1)"; ctx.lineWidth = Math.max(1.5, 2 / scale);
        ctx.beginPath(); ctx.arc(cx, cy, handle * .65, 0, Math.PI * 2); ctx.fill(); ctx.stroke();
        ctx.beginPath(); ctx.arc(hx, hy, handle, 0, Math.PI * 2); ctx.fill(); ctx.stroke(); ctx.restore();
        return result;
      };
    }

    // Critical fix: the replay card may already have built before this helper loads.
    // Patch those existing instances immediately instead of waiting for _buildDom().
    patchExistingCards();
    let retries = 20;
    const timer = setInterval(() => {
      patchExistingCards();
      if (--retries <= 0) clearInterval(timer);
    }, 500);

    console.info(`[OpenNeato] no-go shape tools ${PATCH_VERSION} loaded`);
  });
})();
