/* OpenNeato dashboard spacing helper.
 *
 * The no-go editor stays inside the replay card, where map interaction is
 * reliable. This helper deliberately reserves real layout height under that
 * editor so cards placed below it (schedule, notifications, etc.) cannot
 * crowd or overlap the dynamically injected controls.
 *
 * This replaces the abandoned standalone no-go Lovelace-card experiment.
 */
(() => {
  "use strict";

  const PATCH_VERSION = "1.0.0-reserved-layout";
  const TARGET_TAG = "openneato-replay-card";
  const RESERVED_BOTTOM_PX = 72;
  const EXTRA_CARD_ROWS = 2;

  function applySpacing(card) {
    if (!card?.shadowRoot) return;

    const haCard = card.shadowRoot.querySelector("ha-card");
    const controls = card.shadowRoot.querySelector(".nogo-controls");
    if (!haCard || !controls) return;

    // Keep the no-go controls in normal document flow and give them a stable
    // footprint. The extra bottom padding is intentional reserved space for
    // dashboards that position the following cards before the helper toolbar
    // has finished injecting its buttons.
    controls.style.display = "flex";
    controls.style.minHeight = "74px";
    controls.style.boxSizing = "border-box";
    controls.style.marginBottom = "0";
    haCard.style.boxSizing = "border-box";
    haCard.style.paddingBottom = `${RESERVED_BOTTOM_PX}px`;

    card.dataset.openneatoNogoSpace = PATCH_VERSION;
  }

  function walkOpenRoots(root) {
    if (!root?.querySelectorAll) return;
    for (const node of root.querySelectorAll("*")) {
      if (node.localName === TARGET_TAG) applySpacing(node);
      if (node.shadowRoot) walkOpenRoots(node.shadowRoot);
    }
  }

  function patchExistingCards() {
    if (document.documentElement?.localName === TARGET_TAG) {
      applySpacing(document.documentElement);
    }
    walkOpenRoots(document);
  }

  customElements.whenDefined(TARGET_TAG).then(() => {
    const Card = customElements.get(TARGET_TAG);
    if (!Card) return;
    const proto = Card.prototype;

    // Older/masonry dashboards use getCardSize() to reserve vertical rows
    // before the browser has measured dynamic content. Add two rows (~100 px)
    // for the no-go toolbar and its breathing room.
    if (proto.__openNeatoNogoLayoutPatch !== PATCH_VERSION) {
      proto.__openNeatoNogoLayoutPatch = PATCH_VERSION;
      const originalGetCardSize = proto.getCardSize;
      proto.getCardSize = function (...args) {
        const base = typeof originalGetCardSize === "function"
          ? Number(originalGetCardSize.apply(this, args)) || 1
          : 1;
        return base + EXTRA_CARD_ROWS;
      };

      const originalBuildDom = proto._buildDom;
      if (typeof originalBuildDom === "function") {
        proto._buildDom = function (...args) {
          const result = originalBuildDom.apply(this, args);
          applySpacing(this);
          return result;
        };
      }
    }

    patchExistingCards();

    // The replay card or its helper controls can appear after this file has
    // loaded. A short bounded retry covers dashboard first render without
    // leaving a permanent polling loop behind.
    let retries = 24;
    const timer = setInterval(() => {
      patchExistingCards();
      if (--retries <= 0) clearInterval(timer);
    }, 500);

    console.info(`[OpenNeato] no-go reserved-layout helper ${PATCH_VERSION} loaded`);
  });
})();
