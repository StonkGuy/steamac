-- One row per finished session. Only UTC day precision is retained; no client IP is stored.
-- Unknown names, hardware topology and guest builds are omitted by the launcher.
CREATE TABLE sessions (
    id INTEGER PRIMARY KEY,
    v INTEGER NOT NULL CHECK (v = 1),
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
    received TEXT NOT NULL DEFAULT (date('now'))
) STRICT;
CREATE INDEX sessions_appid ON sessions(appid, id DESC);
CREATE INDEX sessions_config ON sessions(appid, mac_chip, mac_ram_gb, vm_driver, vm_wmtr);
CREATE INDEX sessions_install_day ON sessions(install, received);

CREATE TABLE ratings (
    id INTEGER PRIMARY KEY,
    v INTEGER NOT NULL CHECK (v = 1),
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
    received TEXT NOT NULL DEFAULT (date('now')),
    UNIQUE (install, appid)
) STRICT;
CREATE INDEX ratings_appid ON ratings(appid, id DESC);
CREATE INDEX ratings_config ON ratings(appid, mac_chip, mac_ram_gb, vm_driver, vm_wmtr);

-- Quotas count actual writes (including rating replacement/clearing), not duplicate session retries.
CREATE TABLE daily_usage (
    install TEXT NOT NULL,
    received TEXT NOT NULL,
    requests INTEGER NOT NULL,
    PRIMARY KEY (install, received)
) STRICT;
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

-- Separate latest-known names survive unnamed sessions and rating replacement/clearing.
CREATE TABLE game_names (appid INTEGER PRIMARY KEY, game TEXT NOT NULL) STRICT;
CREATE TRIGGER session_game_name AFTER INSERT ON sessions WHEN NEW.game IS NOT NULL BEGIN
    INSERT INTO game_names VALUES (NEW.appid, NEW.game)
    ON CONFLICT (appid) DO UPDATE SET game = excluded.game;
END;
CREATE TRIGGER rating_game_name AFTER INSERT ON ratings WHEN NEW.game IS NOT NULL BEGIN
    INSERT INTO game_names VALUES (NEW.appid, NEW.game)
    ON CONFLICT (appid) DO UPDATE SET game = excluded.game;
END;
