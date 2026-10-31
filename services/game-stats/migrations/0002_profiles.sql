-- Rebuild to widen the original v=1 CHECK without changing historical reports.
-- Copy before recreating triggers so migration does not consume quotas or rewrite names.

DROP TRIGGER session_quota;

DROP TRIGGER rating_quota;

DROP TRIGGER rating_clear_quota;

DROP TRIGGER session_game_name;

DROP TRIGGER rating_game_name;

CREATE TABLE sessions_v2 (
    id INTEGER PRIMARY KEY,
    v INTEGER NOT NULL CHECK (v IN (1, 2)),
    sid TEXT NOT NULL UNIQUE,
    install TEXT NOT NULL,
    launcher TEXT NOT NULL,
    appid INTEGER NOT NULL,
    game TEXT,
    mac_model TEXT NOT NULL,
    mac_chip TEXT NOT NULL,
    mac_pcores INTEGER,
    mac_cores INTEGER NOT NULL,
    mac_gpu_cores INTEGER,
    mac_ram_gb INTEGER NOT NULL,
    mac_macos TEXT NOT NULL,
    vm_cpus INTEGER NOT NULL,
    vm_mem_mib INTEGER NOT NULL,
    vm_driver TEXT NOT NULL,
    vm_wmtr INTEGER NOT NULL CHECK (vm_wmtr IN (0, 1)),
    vm_hz REAL NOT NULL,
    vm_steamos TEXT,
    launched INTEGER NOT NULL CHECK (launched IN (0, 1)),
    end TEXT NOT NULL,
    run_s REAL NOT NULL,
    play_s REAL NOT NULL,
    first_window_s REAL,
    proton TEXT,
    res TEXT,
    fps_avg REAL,
    fps_p50 REAL,
    fps_low1 REAL,
    fps_hitches_per_min REAL,
    mem_game_mib REAL,
    mem_guest_mib REAL,
    mem_host_mib REAL,
    gfx TEXT CHECK (gfx IN ('vulkan', 'dxmt')),
    env TEXT,
    received TEXT NOT NULL DEFAULT (date('now'))
) STRICT;

INSERT INTO sessions_v2 (id, v, sid, install, launcher, appid, game, mac_model, mac_chip, mac_pcores, mac_cores, mac_gpu_cores, mac_ram_gb, mac_macos, vm_cpus, vm_mem_mib, vm_driver, vm_wmtr, vm_hz, vm_steamos, launched, end, run_s, play_s, first_window_s, proton, res, fps_avg, fps_p50, fps_low1, fps_hitches_per_min, mem_game_mib, mem_guest_mib, mem_host_mib, received) SELECT id, v, sid, install, launcher, appid, game, mac_model, mac_chip, mac_pcores, mac_cores, mac_gpu_cores, mac_ram_gb, mac_macos, vm_cpus, vm_mem_mib, vm_driver, vm_wmtr, vm_hz, vm_steamos, launched, end, run_s, play_s, first_window_s, proton, res, fps_avg, fps_p50, fps_low1, fps_hitches_per_min, mem_game_mib, mem_guest_mib, mem_host_mib, received FROM sessions;

DROP TABLE sessions;

ALTER TABLE sessions_v2 RENAME TO sessions;

CREATE INDEX sessions_appid ON sessions(appid, id DESC);

CREATE INDEX sessions_config ON sessions(appid, mac_chip, mac_ram_gb, vm_driver, coalesce(gfx, CASE WHEN vm_wmtr = 0 THEN 'vulkan' END), env);

CREATE INDEX sessions_install_day ON sessions(install, received);

CREATE INDEX sessions_profile ON sessions(appid, vm_driver, coalesce(gfx, CASE WHEN vm_wmtr = 0 THEN 'vulkan' END), env);

CREATE TABLE ratings_v2 (
    id INTEGER PRIMARY KEY,
    v INTEGER NOT NULL CHECK (v IN (1, 2)),
    install TEXT NOT NULL,
    launcher TEXT NOT NULL,
    appid INTEGER NOT NULL,
    game TEXT,
    rating TEXT NOT NULL CHECK (rating IN ('well', 'issues', 'unplayable', 'no_start')),
    mac_model TEXT NOT NULL,
    mac_chip TEXT NOT NULL,
    mac_pcores INTEGER,
    mac_cores INTEGER NOT NULL,
    mac_gpu_cores INTEGER,
    mac_ram_gb INTEGER NOT NULL,
    mac_macos TEXT NOT NULL,
    vm_driver TEXT NOT NULL,
    vm_wmtr INTEGER NOT NULL CHECK (vm_wmtr IN (0, 1)),
    vm_cpus INTEGER,
    vm_mem_mib INTEGER,
    vm_hz REAL,
    vm_steamos TEXT,
    gfx TEXT CHECK (gfx IN ('vulkan', 'dxmt')),
    env TEXT,
    received TEXT NOT NULL DEFAULT (date('now')),
    UNIQUE (install, appid)
) STRICT;

INSERT INTO ratings_v2 (id, v, install, launcher, appid, game, rating, mac_model, mac_chip, mac_pcores, mac_cores, mac_gpu_cores, mac_ram_gb, mac_macos, vm_driver, vm_wmtr, vm_cpus, vm_mem_mib, vm_hz, vm_steamos, received) SELECT id, v, install, launcher, appid, game, rating, mac_model, mac_chip, mac_pcores, mac_cores, mac_gpu_cores, mac_ram_gb, mac_macos, vm_driver, vm_wmtr, vm_cpus, vm_mem_mib, vm_hz, vm_steamos, received FROM ratings;

DROP TABLE ratings;

ALTER TABLE ratings_v2 RENAME TO ratings;

CREATE INDEX ratings_appid ON ratings(appid, id DESC);

CREATE INDEX ratings_config ON ratings(appid, mac_chip, mac_ram_gb, vm_driver, coalesce(gfx, CASE WHEN vm_wmtr = 0 THEN 'vulkan' END), env);

CREATE INDEX ratings_profile ON ratings(appid, vm_driver, coalesce(gfx, CASE WHEN vm_wmtr = 0 THEN 'vulkan' END), env);

CREATE TRIGGER session_quota BEFORE INSERT ON sessions BEGIN
    INSERT INTO daily_usage VALUES (NEW.install, date('now'), 1)
    ON CONFLICT (install, received) DO UPDATE SET requests = requests + 1 WHERE requests < 200;
    SELECT CASE WHEN changes() = 0 THEN RAISE(ABORT, 'daily rate limit') END;
END;

CREATE TRIGGER rating_quota BEFORE INSERT ON ratings BEGIN
    INSERT INTO daily_usage VALUES (NEW.install, date('now'), 1)
    ON CONFLICT (install, received) DO UPDATE SET requests = requests + 1 WHERE requests < 200;
    SELECT CASE WHEN changes() = 0 THEN RAISE(ABORT, 'daily rate limit') END;
END;

CREATE TRIGGER rating_clear_quota BEFORE DELETE ON ratings BEGIN
    INSERT INTO daily_usage VALUES (OLD.install, date('now'), 1)
    ON CONFLICT (install, received) DO UPDATE SET requests = requests + 1 WHERE requests < 200;
    SELECT CASE WHEN changes() = 0 THEN RAISE(ABORT, 'daily rate limit') END;
END;

CREATE TRIGGER session_game_name AFTER INSERT ON sessions WHEN NEW.game IS NOT NULL BEGIN
    INSERT INTO game_names VALUES (NEW.appid, NEW.game)
    ON CONFLICT (appid) DO UPDATE SET game = excluded.game;
END;

CREATE TRIGGER rating_game_name AFTER INSERT ON ratings WHEN NEW.game IS NOT NULL BEGIN
    INSERT INTO game_names VALUES (NEW.appid, NEW.game)
    ON CONFLICT (appid) DO UPDATE SET game = excluded.game;
END;
