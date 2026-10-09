# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

OBS Studio plugin (C/C++/Qt) that tracks youth hockey scoreboard state and writes it to individual text files. Users compose their scoreboard overlay in OBS using standard Text sources pointed at these files. Provides a dock UI for manual control, OBS hotkeys for hands-free operation, and optional CLI integration for event-driven workflows.

## Build Commands

All commands use `make` wrappers. OBS paths are typically required since `pkg-config` rarely finds libobs:

```bash
# First-time setup
make setup                    # Install brew dependencies
make find-obs-dev-paths       # Discover local OBS header/lib paths

# Common dev cycle (configure + build + test)
make dev OBS_INCLUDE_DIR=... OBS_LIBRARY=... OBS_SOURCE_DIR=...

# Individual steps
make configure PRESET=default OBS_INCLUDE_DIR=... OBS_LIBRARY=...
make build
make test

# Coverage (enforces 100% line coverage on scoreboard-core)
make coverage OBS_INCLUDE_DIR=... OBS_LIBRARY=... OBS_SOURCE_DIR=...

# Install and run in OBS
make install
make run
make check-plugin-log
```

CMake presets: `default` (RelWithDebInfo), `debug` (Debug + -Werror), `ninja`, `coverage` (Debug + gcov, no plugin module).

Run a single test directly: `./build/<test-binary-name>` (e.g. `./build/scoreboard_core_tests`).

## Architecture

Two-layer design separating testable core logic from OBS-dependent code:

**Scoreboard Core** (`src/scoreboard-core.c`, `include/scoreboard-core.h`):
- Pure C static library, no OBS dependencies
- Manages game state: clock (tenths resolution), period, score, shots, faceoffs, penalties, fouls, team names
- Configurable period labels with sport-specific defaults and external file override
- Penalty queue: only `SCOREBOARD_MAX_RUNNING_PENALTIES` (2) tick simultaneously per team; extras auto-start on expiry; slot consolidation via `scoreboard_penalty_compact()` keeps penalties packed to lowest slots
- Compound penalties: `phase2_tenths` field on `scoreboard_penalty` struct enables two-phase penalties (2+2, 2+5, 2+10); phase 2 auto-starts when phase 1 expires or is cleared/edited to 0
- Penalty edit: `scoreboard_*_penalty_set_time()` changes remaining time; setting to 0 on a compound penalty transitions to phase 2 instead of clearing
- Penalty label format: configurable template (`{{ number }}`, `{{ time }}`, `{{ phase2 }}`, `{{ if_phase2 }}...{{ end_if }}`) for combined `home_penalty_labels.txt` / `away_penalty_labels.txt` output files
- Cumulative game clock: opt-in feature tracking total elapsed time across all periods; `game_clock_accumulated_tenths` stores completed period time, `scoreboard_game_clock_get_tenths()` computes live total; configurable display format (MM:SS or H:MM:SS)
- Text file output: writes 38 files (clock, period, scores, shots, faceoffs, fouls, penalties, penalty labels, period labels, sport, penalty durations, period length, cumulative clock, home plus/minus game and season, scoring game and season, last goal, penalty minutes, points per game, faceoff percentage, goalie in net, goalies game and season) to configurable directory; `game_summary.txt` plus a dated copy and `season_backup.txt` are written only by `scoreboard_end_game()`
- Home player roster and stats: only the home team is tracked (up to `SCOREBOARD_MAX_ROSTER` players by jersey number) with plus/minus, goals and assists (no persistent on-ice state: who was on the ice is named per goal); every stat has a game value and a season value (`season_*`), and changing a game value moves the season value by the same amount; `scoreboard_new_game()` clears game values only (`scoreboard_roster_reset_game_stats()`), `scoreboard_roster_reset_season_stats()` clears the season. `scoreboard_increment_*_score()` records the goal (nobody gets +/- yet; goals during any active penalty are marked skipped when `scoreboard_get_plus_minus_skip_power_play()` is set); `scoreboard_set_goal_on_ice()` names who was on the ice for the latest goal by a team (home goal +1, away goal -1) and can be called again to replace the answer; `scoreboard_decrement_*_score()` reverses the most recent matching goal from a bounded history. `scoreboard_credit_goal()` credits the latest home goal (stored in the same `pm_event` history so taking the goal back reverses it) and `scoreboard_player_set_plus_minus/goals/assists/season()` allow manual edits. `scoreboard_roster_to_string()` (`number:0:pm:goals:assists:season_pm:season_goals:season_assists`, the second slot is an unused legacy on-ice flag; older shorter forms still load) / `scoreboard_roster_from_string()` give the dock a compact form to keep the roster in the OBS profile config
- Goalies (`SCOREBOARD_MAX_GOALIES` 4) are separate from the roster: `struct scoreboard_goalie` (shots against, goals against, season values, games). `scoreboard_set_goalie_in_net()` picks who is in net; `scoreboard_increment/decrement_away_shots()` and away goals charge that goalie (the goalie is stored in the `pm_event` so taking a goal back credits the right one). A goal also counts as a shot (`scoreboard_increment_*_score()` adds one to that team's shots, and the goalie's SA for away goals; the decrements take it back). `set_*` totals never touch goalie numbers. Goalies also keep `toi_tenths`/`season_toi_tenths`, added in `scoreboard_clock_tick()` from the time that really passed on the clock. `scoreboard_goalies_to_string/from_string` for the dock config
- Skaters also have `pim`/`season_pim` (added by `scoreboard_home_penalty_add*`, suppressed while `parse_penalty_files` re-reads files via `g_loading_penalties`) and `games`. Each home penalty remembers the minutes it added (`scoreboard_penalty.pim_minutes`) so `scoreboard_home_penalty_remove()` (manual delete) can take them back; natural expiry, goal release and `scoreboard_home_penalty_clear()` leave PIM alone. PPG = finished-game points / games (the game in progress is excluded until End Game)
- `struct scoreboard_penalty.major` (5+ minutes) is kept up to date by `penalty_phase_two()`; `release_home_minor_for_goal()` applies the away-goal rule (2 removed, 4 to 2, 2+2 to phase 2, never majors, only when the home team has more running penalties)
- `scoreboard_end_game(played, count)` adds a game for players and goalies, writes the summary; `scoreboard_reopen_last_game()` undoes it and, via an in-memory snapshot taken by `scoreboard_new_game()` (`g_prev_game`), restores the previous game
- Lineup: `scoreboard_player.out` (not dressed) kept in roster string field 12 and JSON; `scoreboard_player_set_dressed()`. Lines: `scoreboard_line_*` (forward max 3, defence max 2, names F1/D1 from order, `scoreboard_lines_to_string/from_string`). Backup: `scoreboard_export_backup/import_backup`. Reopen memory: `scoreboard_reopen_memory_to_string/from_string` + `scoreboard_reopen_revision()` so the dock can keep `g_prev_game` across OBS restarts. Goalie TOI also shifts in `scoreboard_clock_set_tenths/adjust_*` via `goalie_toi_for_clock_change()`
- Goal on-ice answers are capped at `SCOREBOARD_MAX_ON_ICE` (5)
- JSON state persistence (save/load), action log ring buffer (64 entries)
- All exported functions use `scoreboard_*` prefix
- `scoreboard_reset_state_for_tests()` resets global state between test runs

**OBS Module** (`src/plugin-main.c`):
- Minimal C entry point implementing `obs_module_load`/`obs_module_unload`
- Adapts core log callbacks to OBS logging API
- Initializes the dock UI

**Plugin Metadata** (`data/manifest.json.in`):
- OBS 32+ Plugin Manager metadata (display name, version, URLs)
- Generated at configure time from `data/manifest.json.in` → `build/data/manifest.json`
- Version auto-syncs from `CMakeLists.txt` via `@PROJECT_VERSION@`

**Dock UI** (`src/plugin-dock.cpp`, `include/scoreboard-dock.h`):
- C++17 with Qt5/Qt6 (conditional `QAction` include), scoreboard control interface
- QTimer (100ms) drives clock tick, file writes, and UI updates
- On-ice section (hockey only): one dock button opening a non-modal Home Players window (`open_stats_dialog()`) with the whole roster as fixed-width rows (+/- goals assists points, Game/Season switch, click to edit), a per-goal dialog (`prompt_goal_credit()`) asking who scored and who was on the ice, plus a Roster menu; rosters are saved to the OBS profile config by `persist_rosters_if_changed()` on each tick
- 45 OBS hotkeys for hands-free operation (score, shots, faceoffs, fouls, penalties, penalty edit, compound penalties, clock, period)
- CLI process queue with token expansion (`{event}`, `{home_name}`, `{away_name}`, etc.)
- Game event timestamps: stream-relative (`timestamps.txt`) for YouTube chapter descriptions; uses cumulative game clock time instead of stream time when game clock is enabled
- Recording chapter markers: embeds chapters into Hybrid MP4/MOV via `obs_frontend_recording_add_chapter()` (OBS 32+, resolved at runtime via `dlsym`/`GetProcAddress` for backwards compatibility; only works with Hybrid MP4 output, not Standard/FFmpeg muxer)
- Companion `.chapters.txt` file written next to any recording (all formats) for reeln-cli use
- Sport-aware score labels: hockey/soccer/lacrosse log "Goal", rugby logs "Try", basketball/football disable score logging by default (too frequent)

## Testing

Tests are plain C using `assert()` — no external test framework. Nine test binaries exercising scoreboard-core:

- `test-scoreboard-core.c` — clock, period, lifecycle, cumulative game clock
- `test-scoreboard-core-scoring.c` — score, shots, team names, new game
- `test-scoreboard-core-penalties.c` — penalty add/clear/tick/format, slot compaction, edit/set_time, compound penalties (phase transition, clear, adjust, leftover carry), penalty label format/preview
- `test-scoreboard-core-persistence.c` — file output, JSON save/load, action logs, CLI settings, game clock persistence, penalty label file output
- `test-scoreboard-core-sport.c` — sport presets, fouls, score labels
- `test-scoreboard-core-events.c` — event log add/remove/find/write lifecycle
- `test-scoreboard-core-goalies.c` — goalies, faceoff percent, 5 on ice, PIM, penalty release, PPG, end game, reopen, persistence
- `test-scoreboard-core-plusminus.c` — roster, goal on-ice answers, goal crediting and reversal, penalty skipping, file output, persistence, roster text form

Each test calls `scoreboard_reset_state_for_tests()` for isolation. Tests run via `ctest --preset default` or `make test`.

## Quality Requirements

- **100% line coverage** on `scoreboard-core.c` enforced by `make coverage` / `scripts/coverage.sh`
- Compiler warnings: `-Wall -Wextra -Wpedantic` (errors with `debug` preset)
- Standards: C11 (strict), C++17 (strict), no extensions
- Keep unit-testable logic in scoreboard-core, outside OBS entry points

## Release Process

Releasing a new version requires these steps in order:

1. **Update version** in `CMakeLists.txt` (`project(... VERSION X.Y.Z ...)`) — this automatically updates `plugin-version.h` and `data/manifest.json` via `configure_file()`
2. **Update CHANGELOG.md**: move items from `[Unreleased]` into a new `[X.Y.Z] - YYYY-MM-DD` section
3. **Rebuild and test locally**:
   - `make reconfigure` (picks up new version)
   - `make build && make test`
   - `make coverage` (must pass 100% line coverage on scoreboard-core)
   - `make install` then launch OBS and verify the dock loads, version shows correctly in header and About dialog
4. **Commit** the version bump + changelog update
5. **Tag**: `git tag vX.Y.Z && git push origin vX.Y.Z`
6. The `release.yml` workflow triggers on `v*` tags and builds platform packages (creates a **draft** release)
7. **Publish the release** with user-friendly notes: `gh release edit vX.Y.Z --draft=false --notes "..."`

### Release Notes Format

Release notes should be written for **non-technical users** (streamers, hockey parents), not developers. Use plain language — explain what changed from the user's perspective, not implementation details.

Use these sections as applicable:
- **What's New** — new features and capabilities
- **What's Improved** — enhancements to existing features
- **What's Fixed** — bug fixes described by the symptom the user would have seen
- **What's Changed** — behavioral changes users should be aware of
- **Download** — list each platform's file with a one-line install instruction, and link to the README installation section

Example:
```
## What's Fixed

- **Hotkey bindings now persist across OBS restarts.** Previously, any hotkeys
  you configured in OBS Settings > Hotkeys would be lost every time you closed
  OBS. They now save and restore automatically.

## Download

Pick the file for your platform:
- **macOS:** `streamn-obs-scoreboard-X.Y.Z-macos.pkg` — double-click to install
- **Windows:** `streamn-obs-scoreboard-X.Y.Z-windows-x64.zip` — extract and copy the DLL
- **Linux:** `streamn-obs-scoreboard-X.Y.Z-linux-x86_64.tar.gz` — extract and copy the .so

See the [README](https://github.com/StreamnDad/streamn-scoreboard#installation) for detailed steps.
```

### OBS Forum Update Format

After publishing a GitHub release, post an update to the OBS Forums plugin page. The format is:

**Title:** `[Update] Streamn Scoreboard vX.Y.Z — short summary`

**Body:** Uses the same sections as GitHub release notes (What's Fixed, What's New, What's Improved, What's Changed) but **without** the Download section — the OBS Forums page links to the GitHub release automatically.

Example:
```
[Update] Streamn Scoreboard v0.2.2 — Hotkey persistence fix + macOS installer fix

## What's Fixed

- **macOS installer now works correctly.** The previous releases either placed the
  plugin where OBS couldn't find it, or crashed OBS on startup due to a Qt version
  mismatch. Both issues are resolved — the installer now creates a proper .plugin
  bundle in the right location, built against the correct Qt version.
- **Hotkey bindings now persist across OBS restarts.** Previously, any hotkeys you
  configured in OBS Settings > Hotkeys would be lost every time you closed OBS.
  They now save and restore automatically.

## What's Improved

- **macOS install instructions** now include a note about the Gatekeeper warning —
  right-click the .pkg and choose **Open** to bypass it.
```

## Naming Conventions

- `g_` prefix for globals, `k` prefix for constants
- `scoreboard_*` for exported C API functions
- Event types: `SB_EVENT_HOME_GOAL`, `SB_EVENT_AWAY_GOAL`, etc.
- Struct names: `scoreboard_penalty` for penalty data
