// Shared endpoint and local-testing override for the landing and statistics pages.
(function () {
  "use strict";
  const API = "https://steamac-game-stats.steamac-game-stats.workers.dev";
  function baseURL() {
    const override = new URLSearchParams(location.search).get("api");
    if (!override) return API;
    const url = new URL(override);
    if (!["http:", "https:"].includes(url.protocol) || url.username || url.password) throw new Error("Invalid API URL");
    return url.href.replace(/\/$/, "");
  }
  function gamesURL(appid) {
    const params = new URLSearchParams();
    const override = new URLSearchParams(location.search).get("api");
    if (override) params.set("api", override);
    const query = params.toString();
    return `games.html${query ? `?${query}` : ""}${appid === undefined ? "" : `#${appid}`}`;
  }
  window.steamacStats = Object.freeze({ baseURL, gamesURL });
})();
