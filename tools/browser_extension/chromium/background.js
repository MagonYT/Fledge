// Send to Fledge - background script (docs/ASSET_STORES.md).
// Watches only for finished downloads that came from fab.com or assetstore.unity.com and have a file type Fledge
// imports, then tells that site's tab (content.js), which shows a "Send to Fledge" button. It makes no network
// requests and sends nothing anywhere; the file's local path only ever goes into a fledge:// link the user clicks.
const api = typeof browser !== "undefined" ? browser : chrome;
const IMPORTABLE = /\.(unitypackage|zip|fbx|glb|gltf|obj)$/i;

function storeHost(url) {
  try {
    const h = new URL(url).hostname;
    return h === "fab.com" || h.endsWith(".fab.com") || h === "assetstore.unity.com";
  } catch (e) {
    return false;
  }
}

api.downloads.onChanged.addListener((delta) => {
  if (!delta.state || delta.state.current !== "complete") return;
  Promise.resolve(api.downloads.search({ id: delta.id })).then((items) => {
    const item = items && items[0];
    if (!item || !IMPORTABLE.test(item.filename || "")) return;
    const fromStore = storeHost(item.referrer || "") || storeHost(item.url || "") || storeHost(item.finalUrl || "");
    if (!fromStore) return;  // downloads from other sites are none of this extension's business
    Promise.resolve(api.tabs.query({ url: ["https://www.fab.com/*", "https://fab.com/*", "https://assetstore.unity.com/*"] })).then((tabs) => {
      const tab = (tabs || []).find((t) => t.active) || (tabs || [])[0];
      if (tab && tab.id !== undefined) api.tabs.sendMessage(tab.id, { type: "fledge-download", path: item.filename });
    });
  });
});
