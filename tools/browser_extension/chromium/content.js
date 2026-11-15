// Send to Fledge - content script for fab.com and assetstore.unity.com (docs/ASSET_STORES.md).
//
// What it does:
//   * adds a "Send to Fledge" button on listing pages; it opens fledge://import?url=<this page>&title=<title>;
//   * when you click the store's "Open in Unity" / "Open in Launcher" buttons it asks whether to send the listing to
//     Fledge instead (Cancel keeps the store's normal behaviour);
//   * after a download from this site finishes (reported by background.js), offers "Send to Fledge" for the file:
//     fledge://import-file?path=<the downloaded file>&url=<this page>.
// What it never does: read cookies, tokens or page content beyond the title, make network requests, or send anything
// anywhere. The only output is a fledge:// link that your browser hands to Fledge after its own confirmation prompt.
(() => {
  "use strict";
  const api = typeof browser !== "undefined" ? browser : chrome;

  function storeOf(host) {
    if (host === "fab.com" || host.endsWith(".fab.com")) return "fab";
    if (host === "assetstore.unity.com") return "unity";
    return "";
  }
  function isListing(u) {
    if (storeOf(u.hostname) === "fab") return /\/listings\/[^/]+/.test(u.pathname);
    if (storeOf(u.hostname) === "unity") return /^\/packages\/.+-\d+\/?$/.test(u.pathname);
    return false;
  }
  function listingUrl() {
    const u = new URL(location.href);
    return u.origin + u.pathname;  // no query string or fragment: nothing but the listing address leaves the page
  }
  function pageTitle() {
    const og = document.querySelector('meta[property="og:title"]');
    const t = (og && og.content) || document.title || "";
    return t.replace(/\s*[|\-]\s*(Fab|Unity Asset Store).*$/i, "").trim().slice(0, 200);
  }
  function open(link) {
    // An anchor click: the browser shows its "Open Fledge?" prompt for the external protocol.
    const a = document.createElement("a");
    a.href = link;
    a.rel = "noopener";
    a.style.display = "none";
    document.documentElement.appendChild(a);
    a.click();
    a.remove();
  }
  function sendListing() {
    const p = new URLSearchParams({ url: listingUrl(), title: pageTitle() });
    open("fledge://import?" + p.toString().replace(/\+/g, "%20"));
  }

  function button(label, onClick, bottom) {
    const b = document.createElement("button");
    b.textContent = label;
    b.type = "button";
    b.setAttribute("data-fledge", "1");
    Object.assign(b.style, {
      position: "fixed", right: "18px", bottom: bottom + "px", zIndex: 2147483646, padding: "9px 14px",
      background: "#e8664f", color: "#fff", border: "0", borderRadius: "8px", font: "600 13px system-ui, sans-serif",
      boxShadow: "0 2px 10px rgba(0,0,0,.35)", cursor: "pointer"
    });
    b.addEventListener("click", (e) => { e.preventDefault(); e.stopPropagation(); onClick(b); });
    document.documentElement.appendChild(b);
    return b;
  }

  let sendButton = null;
  function refresh() {
    const listing = isListing(new URL(location.href));
    if (listing && !sendButton) sendButton = button("Send to Fledge", sendListing, 18);
    if (!listing && sendButton) { sendButton.remove(); sendButton = null; }
  }
  refresh();
  // single-page sites change the URL without reloading
  let last = location.href;
  setInterval(() => { if (location.href !== last) { last = location.href; refresh(); } }, 1000);

  // "Open in Unity" / "Open in Launcher": these are links to the store apps' URL schemes.
  document.addEventListener("click", (e) => {
    const a = e.target && e.target.closest ? e.target.closest("a[href]") : null;
    if (!a || a.hasAttribute("data-fledge")) return;
    const href = a.getAttribute("href") || "";
    if (!/^(com\.unity3d\.kharma:|com\.epicgames\.launcher:)/i.test(href)) return;
    if (/^com\.epicgames\.launcher:\/*apps\//i.test(href)) return;  // game launches stay with the launcher
    if (window.confirm("Send this asset to Fledge instead?\n\nOK: Fledge.   Cancel: the store's own app.")) {
      e.preventDefault();
      e.stopPropagation();
      sendListing();
    }
  }, true);

  // Finished downloads from this site (background.js): offer them to Fledge.
  api.runtime.onMessage.addListener((msg) => {
    if (!msg || msg.type !== "fledge-download" || typeof msg.path !== "string") return;
    const name = msg.path.split(/[\\/]/).pop();
    const b = button("Send " + name + " to Fledge", (self) => {
      const p = new URLSearchParams({ path: msg.path, url: isListing(new URL(location.href)) ? listingUrl() : "" });
      open("fledge://import-file?" + p.toString().replace(/\+/g, "%20"));
      self.remove();
    }, 64);
    setTimeout(() => b.remove(), 60000);
  });
})();
