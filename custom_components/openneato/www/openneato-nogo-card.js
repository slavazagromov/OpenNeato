/* OpenNeato standalone no-go Lovelace card. */
(() => {
  "use strict";

  const CARD_VERSION = "1.0.0";
  const TARGET_TAG = "openneato-replay-card";

  function walkOpenRoots(root, out) {
    if (!root?.querySelectorAll) return;
    for (const node of root.querySelectorAll("*")) {
      if (node.localName === TARGET_TAG) out.push(node);
      if (node.shadowRoot) walkOpenRoots(node.shadowRoot, out);
    }
  }

  function allReplayCards() {
    const out = [];
    if (document.documentElement?.localName === TARGET_TAG) out.push(document.documentElement);
    walkOpenRoots(document, out);
    return [...new Set(out)];
  }

  function matchingReplayCard(entryId) {
    const cards = allReplayCards();
    if (entryId) {
      return cards.find((card) => card?._config?.entry_id === entryId) || null;
    }
    return cards.length === 1 ? cards[0] : null;
  }

  class OpenNeatoNoGoCard extends HTMLElement {
    static getStubConfig() {
      return { type: "custom:openneato-nogo-card", title: "No-go zones" };
    }

    constructor() {
      super();
      this.attachShadow({ mode: "open" });
      this._config = {};
      this._hass = null;
      this._target = null;
      this._timer = null;
      this._hiddenControls = null;
      this._build();
    }

    setConfig(config) {
      this._config = { title: "No-go zones", ...config };
      this._render();
      this._linkTarget();
    }

    set hass(hass) {
      this._hass = hass;
      this._linkTarget();
      this._render();
    }

    connectedCallback() {
      this._linkTarget();
      clearInterval(this._timer);
      this._timer = setInterval(() => {
        this._linkTarget();
        this._render();
      }, 500);
    }

    disconnectedCallback() {
      clearInterval(this._timer);
      this._timer = null;
      this._restoreEmbeddedControls();
      this._target = null;
    }

    getCardSize() {
      return this._target?._nogoEditing ? 3 : 2;
    }

    _build() {
      this.shadowRoot.innerHTML = `
        <style>
          :host { display: block; }
          ha-card { padding: 14px 16px 12px; }
          .title {
            font-size: 1rem;
            font-weight: 600;
            color: var(--primary-text-color);
            margin-bottom: 10px;
          }
          .row {
            display: flex;
            align-items: center;
            gap: 7px;
            flex-wrap: wrap;
          }
          button {
            appearance: none;
            border: 1px solid var(--divider-color);
            border-radius: 10px;
            padding: 7px 10px;
            background: var(--ha-card-background, var(--card-background-color));
            color: var(--primary-text-color);
            font: inherit;
            font-size: .82rem;
            cursor: pointer;
          }
          button:hover { background: var(--secondary-background-color); }
          button.primary {
            border-color: var(--primary-color);
            color: var(--primary-color);
          }
          button.danger, button.guard.active {
            border-color: var(--error-color, #db4437);
            color: var(--error-color, #db4437);
          }
          button.mode.active {
            border-color: var(--primary-color);
            color: var(--primary-color);
            background: color-mix(in srgb, var(--primary-color) 12%, transparent);
          }
          button[hidden], .editing[hidden] { display: none !important; }
          .editing { margin-top: 8px; }
          .note {
            margin-top: 9px;
            color: var(--secondary-text-color);
            font-size: .76rem;
            line-height: 1.35;
          }
          .warning {
            color: var(--warning-color, #ff9800);
          }
        </style>
        <ha-card>
          <div class="title"></div>
          <div class="row base">
            <button class="edit primary">Edit zones</button>
            <button class="guard">Guard on</button>
          </div>
          <div class="row editing" hidden>
            <button class="mode line">Line</button>
            <button class="mode box">▢ Box</button>
            <button class="mode circle">⭕ Circle</button>
            <button class="mode move">✋ Move</button>
            <button class="new-line">New line</button>
            <button class="undo">Undo</button>
            <button class="save primary">Save</button>
            <button class="cancel">Cancel</button>
          </div>
          <div class="note"></div>
        </ha-card>
      `;

      const q = (s) => this.shadowRoot.querySelector(s);
      this._title = q(".title");
      this._edit = q(".edit");
      this._guard = q(".guard");
      this._editing = q(".editing");
      this._note = q(".note");
      this._newLine = q(".new-line");
      this._undo = q(".undo");
      this._save = q(".save");
      this._cancel = q(".cancel");
      this._modes = {
        line: q(".line"), box: q(".box"), circle: q(".circle"), move: q(".move"),
      };

      this._edit.addEventListener("click", () => this._invoke("_beginNoGoEdit"));
      this._guard.addEventListener("click", () => this._invoke("_toggleNoGoEnabled"));
      this._newLine.addEventListener("click", () => this._invoke("_finishNoGoLine"));
      this._undo.addEventListener("click", () => this._invoke("_undoNoGoPoint"));
      this._save.addEventListener("click", () => this._invoke("_saveNoGo"));
      this._cancel.addEventListener("click", () => this._invoke("_cancelNoGoEdit"));
      for (const [mode, button] of Object.entries(this._modes)) {
        button.addEventListener("click", () => this._setMode(mode));
      }
    }

    _linkTarget() {
      const next = matchingReplayCard(this._config?.entry_id);
      if (next === this._target) {
        if (next) this._hideEmbeddedControls(next);
        return;
      }
      this._restoreEmbeddedControls();
      this._target = next;
      if (next) this._hideEmbeddedControls(next);
      this._render();
    }

    _hideEmbeddedControls(card) {
      const controls = card?.shadowRoot?.querySelector(".nogo-controls");
      if (!controls) return;
      if (this._hiddenControls && this._hiddenControls !== controls) this._restoreEmbeddedControls();
      if (!this._hiddenControls) {
        this._hiddenControls = controls;
        this._hiddenDisplay = controls.style.display;
      }
      controls.style.display = "none";
    }

    _restoreEmbeddedControls() {
      if (!this._hiddenControls) return;
      this._hiddenControls.style.display = this._hiddenDisplay || "";
      this._hiddenControls = null;
      this._hiddenDisplay = "";
    }

    async _invoke(method) {
      const target = this._target;
      if (!target || typeof target[method] !== "function") return;
      try {
        await target[method]();
      } catch (err) {
        console.error(`[OpenNeato] no-go card ${method} failed`, err);
      }
      this._render();
    }

    _setMode(mode) {
      const target = this._target;
      if (!target?._nogoEditing) return;
      if (mode !== "line" && target._nogoCurrent?.length) {
        if (target._nogoCurrent.length >= 2) target._nogoDraft.push(target._nogoCurrent);
        target._nogoCurrent = [];
      }
      target._nogoShapeMode = mode;
      if (mode !== "line") target._nogoCurrent = [];
      const messages = {
        line: "Tap points on the map for a no-go line",
        box: "Drag on the map to place and size a no-go box",
        circle: "Drag on the map to place and size a circular no-go area",
        move: "Drag a box/circle to move it; drag the white handle to resize",
      };
      if (target._nogoNote) target._nogoNote.textContent = messages[mode];
      target._dirty = true;
      target._scheduleRender?.();
      this._render();
    }

    _render() {
      if (!this._title) return;
      this._title.textContent = this._config?.title || "No-go zones";
      const target = this._target;
      if (!target) {
        this._editing.hidden = true;
        this._guard.hidden = true;
        this._edit.hidden = false;
        this._edit.disabled = true;
        const multiple = allReplayCards().length > 1 && !this._config?.entry_id;
        this._note.classList.add("warning");
        this._note.textContent = multiple
          ? "More than one OpenNeato map is on this view. Set entry_id on this no-go card to match the replay card."
          : "Waiting for the OpenNeato map card on this view…";
        return;
      }

      this._edit.disabled = false;
      this._note.classList.remove("warning");
      const editing = Boolean(target._nogoEditing);
      const mode = target._nogoShapeMode || "line";
      const enabled = Boolean(target._nogoEnabled);

      this._edit.hidden = editing;
      this._guard.hidden = editing;
      this._editing.hidden = !editing;
      this._guard.textContent = enabled ? "Guard on" : "Guard off";
      this._guard.classList.toggle("active", enabled);
      for (const [name, button] of Object.entries(this._modes)) {
        button.classList.toggle("active", editing && mode === name);
      }
      this._newLine.hidden = !editing || mode !== "line";

      if (editing) {
        this._note.textContent = target._nogoNote?.textContent || "Edit the zones on the map, then Save.";
      } else {
        this._note.textContent = enabled
          ? "No-go guard is enabled. Edit zones to change the saved barriers."
          : "No-go guard is off. Edit zones to draw or adjust barriers.";
      }
    }
  }

  if (!customElements.get("openneato-nogo-card")) {
    customElements.define("openneato-nogo-card", OpenNeatoNoGoCard);
  }

  window.customCards = window.customCards || [];
  if (!window.customCards.some((card) => card.type === "openneato-nogo-card")) {
    window.customCards.push({
      type: "openneato-nogo-card",
      name: "OpenNeato No-go zones",
      description: "Standalone movable controls for drawing and managing OpenNeato no-go zones.",
      preview: false,
    });
  }

  console.info(`[OpenNeato] standalone no-go card ${CARD_VERSION} loaded`);
})();
