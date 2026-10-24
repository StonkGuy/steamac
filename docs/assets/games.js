(function () {
  "use strict";
  const root = document.documentElement;
  const $ = (id) => document.getElementById(id);
  const gameName = (game) => game.game || `App ${game.appid}`;
  const words = {
    game: ["Game", "Игра", "游戏"], sessions: ["Sessions", "Сеансы", "会话"], installs: ["Installs", "Установки", "安装数"],
    launch_rate: ["Launch rate", "Запуски", "启动率"], fps_avg_median: ["Median FPS", "Медиана FPS", "FPS 中位数"],
    chips: ["Tested on", "Проверено на", "已测试芯片"],
    play_hours: ["Play hours", "Часы игры", "游戏小时"], last_seen: ["Last seen", "Последний сеанс", "最近日期"],
    ratings: ["Player ratings", "Оценки игроков", "玩家评分"], well: ["Runs well", "Работает хорошо", "运行良好"],
    issues: ["Playable with issues", "Есть проблемы", "可玩但有问题"], unplayable: ["Unplayable", "Неиграбельно", "无法游玩"], no_start: ["Does not start", "Не запускается", "无法启动"],
    chip: ["Chip", "Чип", "芯片"], ram: ["RAM", "ОЗУ", "内存"], driver: ["Driver", "Драйвер", "驱动"],
    gfx: ["Graphics path", "Графический путь", "图形路径"], env: ["Launch words", "Параметры запуска", "启动参数"],
    no_recommendation: ["No profile has enough reports yet.", "Пока недостаточно отчётов для рекомендации.", "暂无报告充足的推荐配置。"],
    players: ["Players", "Игроки", "玩家"], unknown: ["Unknown", "Неизвестно", "未知"],
    no_env: ["None", "Нет", "无"], vulkan: ["Vulkan", "Vulkan", "Vulkan"], dxmt: ["DXMT", "DXMT", "DXMT"],
    early: ["Early exit", "Ранний выход", "提前退出"], low1: ["1% low FPS", "1% low FPS", "1% 低帧率"],
    game_mem: ["Game MiB", "Игра МиБ", "游戏 MiB"], host_mem: ["VM process MiB", "Процесс ВМ МиБ", "虚拟机进程 MiB"],
    loading: ["Loading statistics…", "Загрузка статистики…", "正在加载统计…"], empty: ["No sessions or ratings have been shared yet.", "Сеансы и оценки пока не опубликованы.", "暂无分享的会话或评分。"],
    no_match: ["No games match your search.", "По вашему запросу игр нет.", "没有匹配的游戏。"],
    filtered_empty: ["No reports match My Mac. Increase the limits or turn the filter off.", "Нет отчётов для «Моего Mac». Увеличьте ограничения или выключите фильтр.", "没有符合「我的 Mac」的报告。请提高上限或关闭筛选。"],
    filtered_missing: ["No reports for this game match My Mac.", "Для этой игры нет отчётов, подходящих «Моему Mac».", "此游戏没有符合「我的 Mac」的报告。"],
    filter_off: ["All reported Macs · filter off", "Все Mac · фильтр выключен", "所有 Mac · 筛选已关闭"],
    no_limits: ["No hardware limits set", "Ограничения не заданы", "未设置硬件上限"],
    gpu_limit: ["GPU cores", "ядер GPU", "GPU 核心"],
    ram_limit: ["GB RAM", "ГБ ОЗУ", "GB 内存"],
    pcores_limit: ["P-cores", "P-ядер", "性能核心"],
    error: ["Could not load statistics. Please try again.", "Не удалось загрузить статистику. Попробуйте ещё раз.", "无法加载统计，请重试。"],
    missing: ["No reports for this game yet.", "Для этой игры пока нет данных.", "该游戏暂无报告。"], retry: ["Retry", "Повторить", "重试"],
    updated: ["Updated", "Обновлено", "更新时间"], yes: ["Yes", "Да", "是"], no: ["No", "Нет", "否"],
  };
  const languages = ["en", "ru", "zh"];
  const ratingKeys = ["well", "issues", "unplayable", "no_start"];
  const text = (key) => words[key]?.[Math.max(0, languages.indexOf(root.dataset.lang))] || key;
  function translated(key) {
    const fragment = document.createDocumentFragment();
    languages.forEach((lang, index) => {
      const span = document.createElement("span");
      span.lang = lang;
      span.textContent = words[key]?.[index] || key;
      fragment.append(span);
    });
    return fragment;
  }
  const number = (value, digits = 0) => value === null || value === undefined ? "—" : Number(value).toLocaleString(root.lang, { maximumFractionDigits: digits });
  const percent = (value) => value === null || value === undefined ? "—" : `${number(value * 100, 1)}%`;
  function cell(value) {
    const td = document.createElement("td");
    if (value instanceof Node) td.append(value); else td.textContent = value;
    return td;
  }
  function headings(id, keys) {
    const tr = document.createElement("tr");
    for (const key of keys) { const th = document.createElement("th"); th.scope = "col"; th.append(translated(key)); tr.append(th); }
    $(id).querySelector("thead").replaceChildren(tr);
  }
  function row(table, values) {
    const tr = document.createElement("tr");
    values.forEach((value) => tr.append(cell(value)));
    table.append(tr);
  }
  function ratingBar(ratings = {}) {
    const wrap = document.createElement("div"), bar = document.createElement("div"), count = document.createElement("span");
    const total = ratingKeys.reduce((sum, key) => sum + (ratings[key] || 0), 0);
    bar.className = "rating-bar";
    bar.setAttribute("role", "img");
    const description = ratingKeys.map((key) => `${text(key)}: ${number(ratings[key] || 0)}`).join(" · ");
    bar.setAttribute("aria-label", description);
    wrap.title = description;
    for (const key of ratingKeys) {
      const part = document.createElement("span"); part.className = `rating-${key}`;
      part.style.width = `${total ? (ratings[key] || 0) * 100 / total : 0}%`;
      bar.append(part);
    }
    count.className = "rating-count";
    count.textContent = total ? `${number(total)} · ${ratingKeys.map((key) => number(ratings[key] || 0)).join(" / ")}` : "—";
    wrap.append(bar, count);
    return wrap;
  }
  function chipList(chips) {
    const label = document.createElement("span");
    label.textContent = chips.slice(0, 4).map((chip) => chip.replace(/^Apple /, "")).join(", ") || "—";
    if (chips.length > 4) label.append(` +${chips.length - 4}`);
    label.title = chips.join(", ");
    return label;
  }
  function status(id, key, retry) {
    const element = $(id);
    element.replaceChildren(translated(key)); element.hidden = false;
    if (retry) { const button = document.createElement("button"); button.type = "button"; button.className = "stats-retry"; button.append(translated("retry")); button.addEventListener("click", retry); element.append(button); }
  }
  let base, games = [], generated = "", detailData = null, detailRequest = 0, listRequest = 0;
  try { base = window.steamacStats.baseURL(); } catch { status("list-status", "error"); return; }
  const hardware = [
    { field: "gpu", param: "max_gpu_cores", max: 4096, label: "gpu_limit" },
    { field: "ram", param: "max_ram_gb", max: 4096, label: "ram_limit" },
    { field: "pcores", param: "max_pcores", max: 1024, label: "pcores_limit" },
  ];
  let myMac = { enabled: false, chip: "", gpu: "", ram: "", pcores: "" };
  function hardwareValue(value, max) {
    if (!["string", "number"].includes(typeof value) || value === "") return "";
    const n = Number(value);
    return Number.isInteger(n) && n >= 1 && n <= max ? String(n) : "";
  }
  function filterQuery() {
    const params = new URLSearchParams();
    if (myMac.enabled) for (const { field, param } of hardware) {
      if (myMac[field]) params.set(param, myMac[field]);
    }
    return params.toString();
  }
  function renderHardwareSummary() {
    if (!myMac.enabled) { $("my-mac-summary").textContent = text("filter_off"); return; }
    const parts = myMac.chip ? [myMac.chip] : [];
    for (const { field, label } of hardware) {
      if (myMac[field]) parts.push(`≤ ${number(myMac[field])} ${text(label)}`);
    }
    if (!filterQuery()) parts.push(text("no_limits"));
    $("my-mac-summary").textContent = parts.join(" · ");
  }
  function saveMyMac() {
    try { localStorage.setItem("myMac", JSON.stringify(myMac)); } catch { /* Private mode. */ }
  }
  function initMyMac() {
    let saved = null;
    try { saved = JSON.parse(localStorage.getItem("myMac")); } catch { /* Missing or unavailable storage. */ }
    const params = new URLSearchParams(location.search);
    const fromURL = ["chip", ...hardware.map(({ field }) => field)].some((key) => params.has(key));
    const source = fromURL ? Object.fromEntries(params) : saved;
    if (source && typeof source === "object") {
      myMac.enabled = fromURL || source.enabled === true;
      myMac.chip = typeof source.chip === "string" ? source.chip.slice(0, 64) : "";
      for (const { field, max } of hardware) myMac[field] = hardwareValue(source[field], max);
    }
    $("my-mac-enabled").checked = myMac.enabled;
    $("my-chip").value = myMac.chip;
    for (const { field } of hardware) $(`my-${field}`).value = myMac[field];
    saveMyMac(); renderHardwareSummary();
    $("my-mac").addEventListener("submit", (event) => event.preventDefault());
    $("my-mac").addEventListener("change", () => {
      if (!$("my-mac").reportValidity()) return;
      const previous = filterQuery(), wasEnabled = myMac.enabled;
      myMac.enabled = $("my-mac-enabled").checked;
      myMac.chip = $("my-chip").value.trim();
      for (const { field, max } of hardware) myMac[field] = hardwareValue($(`my-${field}`).value, max);
      saveMyMac(); renderHardwareSummary();
      if (filterQuery() !== previous || wasEnabled !== myMac.enabled) { load(); route(false); }
    });
  }
  async function fetchJSON(path) {
    const query = filterQuery();
    const response = await fetch(`${base}${path}${query ? `?${query}` : ""}`);
    if (!response.ok) { const error = new Error("API request failed"); error.status = response.status; throw error; }
    return response.json();
  }
  function renderList() {
    if (!generated) return;
    headings("games-table", ["game", "chips", "sessions", "installs", "launch_rate", "fps_avg_median", "play_hours", "last_seen", "ratings"]);
    const body = $("games-table").querySelector("tbody"); body.replaceChildren();
    const query = $("search").value.trim().toLocaleLowerCase(root.lang), sort = $("sort").value;
    const filtered = games.filter((game) => `${gameName(game)} ${game.appid} ${game.chips.join(" ")}`.toLocaleLowerCase(root.lang).includes(query));
    filtered.sort((a, b) => sort === "game" ? gameName(a).localeCompare(gameName(b), root.lang) : sort === "last_seen" ? (b.last_seen || "").localeCompare(a.last_seen || "") : (b[sort] ?? -1) - (a[sort] ?? -1) || a.appid - b.appid);
    for (const game of filtered) {
      const link = document.createElement("a"); link.href = `#${game.appid}`; link.textContent = gameName(game);
      row(body, [link, chipList(game.chips), number(game.sessions), number(game.installs), percent(game.launch_rate), number(game.fps_avg_median, 1), number(game.play_hours, 1), game.last_seen || "—", ratingBar(game.ratings)]);
    }
    $("games-table").hidden = !filtered.length;
    if (filtered.length) $("list-status").hidden = true; else status("list-status", games.length ? "no_match" : filterQuery() ? "filtered_empty" : "empty");
    $("updated").textContent = generated ? `${text("updated")}: ${new Date(generated).toLocaleString(root.lang)}` : "";
  }
  function renderDetail() {
    if (!detailData) return;
    const game = detailData;
    $("detail-title").textContent = `${gameName(game)} · ${game.appid}`;
    const summary = $("detail-summary"); summary.replaceChildren();
    for (const [key, value] of [["sessions", number(game.sessions)], ["installs", number(game.installs)], ["launch_rate", percent(game.launch_rate)], ["play_hours", number(game.play_hours, 1)]]) {
      const item = document.createElement("span"), strong = document.createElement("strong");
      strong.textContent = `${value} `; item.append(strong, translated(key)); summary.append(item);
    }
    summary.append(ratingBar(game.ratings));
    const recommendation = $("recommended-profile"); recommendation.replaceChildren();
    const profile = game.recommended;
    if (!profile) recommendation.append(translated("no_recommendation"));
    else {
      for (const [key, value] of [["driver", profile.driver], ["gfx", translated(profile.gfx)], ["env", profile.env || translated("no_env")], ["players", number(profile.installs)], ["sessions", number(profile.sessions)], ["launch_rate", percent(profile.launch_rate)], ["fps_avg_median", number(profile.fps_avg_median, 1)]]) {
        const item = document.createElement("span"), strong = document.createElement("strong");
        item.append(translated(key), ": ");
        if (value instanceof Node) strong.append(value); else strong.textContent = value;
        item.append(strong); recommendation.append(item);
      }
      const ratings = document.createElement("span"); ratings.append(translated("ratings"), ratingBar(profile.ratings)); recommendation.append(ratings);
    }
    headings("configs-table", ["chip", "ram", "driver", "gfx", "env", "WMTR", "sessions", "installs", "launch_rate", "early", "fps_avg_median", "low1", "game_mem", "host_mem", "play_hours", "ratings"]);
    const configs = $("configs-table").querySelector("tbody"); configs.replaceChildren();
    for (const c of game.configs) row(configs, [c.chip, `${number(c.ram_gb)} GB`, c.driver, translated(c.gfx || "unknown"), c.env || translated("no_env"), translated(c.wmtr ? "yes" : "no"), number(c.sessions), number(c.installs), percent(c.launch_rate), percent(c.early_exit_rate), number(c.fps_avg_median, 1), number(c.fps_low1_median, 1), number(c.game_mib_median), number(c.host_mib_median), number(c.play_hours, 1), ratingBar(c.ratings)]);
    $("detail-status").hidden = true; $("detail-content").hidden = false;
  }
  async function route(scroll = true) {
    const request = ++detailRequest, hash = location.hash.slice(1);
    detailData = null; $("detail-content").hidden = true;
    if (!hash) { $("detail").hidden = true; return; }
    $("detail").hidden = false; $("detail-title").textContent = /^\d+$/.test(hash) ? `Steam · ${hash}` : "Steam";
    if (!/^[1-9]\d*$/.test(hash) || Number(hash) > 4294967295) { status("detail-status", "missing"); return; }
    status("detail-status", "loading");
    try {
      const data = await fetchJSON(`/v1/games/${hash}`);
      if (request !== detailRequest) return;
      detailData = data; renderDetail(); if (scroll) $("detail").scrollIntoView({ block: "start" });
    } catch (error) { if (request === detailRequest) status("detail-status", error.status === 404 ? filterQuery() ? "filtered_missing" : "missing" : "error", route); }
  }
  async function load() {
    const request = ++listRequest;
    generated = ""; games = []; $("updated").textContent = "";
    status("list-status", "loading"); $("games-table").hidden = true;
    try {
      const data = await fetchJSON("/v1/games");
      if (request !== listRequest) return;
      games = data.games; generated = data.generated; renderList();
    } catch { if (request === listRequest) status("list-status", "error", load); }
  }
  function renderControls() {
    const selected = $("sort").value || "sessions";
    $("sort").replaceChildren();
    for (const key of ["sessions", "game", "installs", "launch_rate", "fps_avg_median", "play_hours", "last_seen"]) { const option = document.createElement("option"); option.value = key; option.textContent = text(key); $("sort").append(option); }
    $("sort").value = selected;
    $("rating-legend").replaceChildren();
    for (const key of ratingKeys) { const label = document.createElement("span"), dot = document.createElement("i"); dot.className = `rating-dot rating-${key}`; label.append(dot, translated(key)); $("rating-legend").append(label); }
  }
  document.querySelectorAll("[data-set-lang]").forEach((button) => button.addEventListener("click", () => {
    const lang = button.dataset.setLang; root.dataset.lang = lang; root.lang = lang;
    try { localStorage.setItem("lang", lang); } catch { /* Private mode. */ }
    window.dispatchEvent(new Event("langchange"));
  }));
  window.addEventListener("langchange", () => { renderControls(); renderHardwareSummary(); if (generated) renderList(); renderDetail(); });
  $("search").addEventListener("input", renderList); $("sort").addEventListener("change", renderList);
  window.addEventListener("hashchange", route);
  initMyMac(); renderControls(); load(); route();
})();
