/* OpenNeato replay-card no-go layout helper.
 *
 * Keep the editor attached to the replay card, but place the whole no-go
 * controls panel above the map. That keeps it in normal document flow and
 * avoids the dashboard cards below the map colliding with a dynamically
 * injected toolbar.
 */
(() => {
  "use strict";

  const PATCH_VERSION = "1.1.0-nogo-above-map";
  const TARGET_TAG = "openneato-replay-card";

  function placeNoGoAboveMap(card) {
    const root = card?.shadowRoot;
    if (!root) return;

    const haCard = root.querySelector("ha-card");
    const controls = root.querySelector(".nogo-controls");
    const stage = root.querySelector(".stage");
    if (!haCard || !controls || !stage) return;

    // Undo the previous reserved-space experiment. The no-go panel now has a
    // real position in the card's flow, immediately before the map stage.
    haCard.style.removeProperty("padding-bottom");
    haCard.style.removeProperty("box-sizing");
    controls.style.removeProperty("display");
    controls.style.removeProperty("min-height");
    controls.style.removeProperty("box-sizing");
    controls.style.removeProperty("margin-bottom");

    if (controls.nextElementSibling !== stage) {
      haCard.insertBefore(controls, stage);
    }

    card.dataset.openneatoNogoLayout = PATCH_VERSION;
  }

  function walkOpenRoots(root) {
    if (!root?.querySelectorAll) return;
    for (const node of root.querySelectorAll("*")) {
      if (node.localName === TARGET_TAG) placeNoGoAboveMap(node);
      if (node.shadowRoot) walkOpenRoots(node.shadowRoot);
    }
  }

  function patchExistingCards() {
    if (document.documentElement?.localName === TARGET_TAG) {
      placeNoGoAboveMap(document.documentElement);
    }
    walkOpenRoots(document);
  }

  customElements.whenDefined(TARGET_TAG).then(() => {
    const Card = customElements.get(TARGET_TAG);
    if (!Card) return;
    const proto = Card.prototype;

    if (proto.__openNeatoNogoLayoutPatch !== PATCH_VERSION) {
      proto.__openNeatoNogoLayoutPatch = PATCH_VERSION;

      const originalBuildDom = proto._buildDom;
      if (typeof originalBuildDom === "function") {
        proto._buildDom = function (...args) {
          const result = originalBuildDom.apply(this, args);
          placeNoGoAboveMap(this);
          return result;
        };
      }
    }

    patchExistingCards();

    // Cover cards created after this helper loads during the first dashboard
    // render without leaving a permanent polling loop.
    let retries = 24;
    const timer = setInterval(() => {
      patchExistingCards();
      if (--retries <= 0) clearInterval(timer);
    }, 500);

    console.info(`[OpenNeato] no-go above-map helper ${PATCH_VERSION} loaded`);
  });
})();
