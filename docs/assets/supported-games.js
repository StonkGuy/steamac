// Public compatibility highlights are derived from aggregate reports, never individual sessions.
(function () {
  "use strict";
  const grid = document.getElementById("supported-games-grid");
  const empty = document.getElementById("supported-games-empty");
  const error = document.getElementById("supported-games-error");
  const all = document.getElementById("supported-games-all");
  const words = {
    well: ["Runs well", "Работает хорошо", "运行良好"],
    playable: ["Playable", "Можно играть", "可游玩"],
    fps: ["median FPS", "медиана FPS", "FPS 中位数"],
    chips: ["Tested on", "Проверено на", "已测试芯片"],
    sessions: ["Sessions", "Сеансы", "会话"],
  };
  let games = null;
  function translated(key) {
    const fragment = document.createDocumentFragment();
    ["en", "ru", "zh"].forEach((lang, index) => {
      const span = document.createElement("span");
      span.lang = lang; span.textContent = words[key][index]; fragment.append(span);
    });
    return fragment;
  }
  function totals(game) {
    const ratings = game.ratings;
    const positive = ratings.well + ratings.issues;
    const negative = ratings.unplayable + ratings.no_start;
    return { positive, negative, total: positive + negative };
  }
  function render() {
    if (games === null) return;
    const supported = games.filter((game) => game.launch_rate > 0 && totals(game).positive >= totals(game).negative)
      .sort((a, b) => b.sessions - a.sessions || a.appid - b.appid).slice(0, 12);
    grid.replaceChildren();
    for (const game of supported) {
      const ratings = totals(game);
      const well = ratings.total ? game.ratings.well > ratings.total / 2 : game.launch_rate >= 0.9;
      const card = document.createElement("a"), name = document.createElement("h4"), badge = document.createElement("span");
      card.className = "card supported-game"; card.href = window.steamacStats.gamesURL(game.appid);
      name.textContent = game.game || `App ${game.appid}`;
      badge.className = `supported-badge${well ? " supported-well" : ""}`; badge.append(translated(well ? "well" : "playable"));
      const fps = document.createElement("p"), value = document.createElement("strong");
      value.textContent = game.fps_avg_median === null ? "—" : game.fps_avg_median.toLocaleString(document.documentElement.lang, { maximumFractionDigits: 1 });
      fps.append(value, " ", translated("fps"));
      const chips = document.createElement("p");
      const short = game.chips.slice(0, 3).map((chip) => chip.replace(/^Apple /, "")).join(", ") || "—";
      chips.append(translated("chips"), `: ${short}${game.chips.length > 3 ? ` +${game.chips.length - 3}` : ""}`);
      chips.title = game.chips.join(", ");
      const sessions = document.createElement("p");
      sessions.append(translated("sessions"), `: ${game.sessions.toLocaleString(document.documentElement.lang)}`);
      card.append(name, badge, fps, chips, sessions); grid.append(card);
    }
    grid.hidden = !supported.length; empty.hidden = Boolean(supported.length); error.hidden = true;
  }
  window.addEventListener("langchange", render);
  (async () => {
    try {
      all.href = window.steamacStats.gamesURL();
      const response = await fetch(`${window.steamacStats.baseURL()}/v1/games`);
      if (!response.ok) throw new Error("Statistics unavailable");
      games = (await response.json()).games; render();
    } catch {
      grid.hidden = true; empty.hidden = true; error.hidden = false;
    }
  })();
})();
