# CPR-vCodex Agent Guide

CPR-vCodex is a CrossPoint Reader fork for the Xteink X4 e-reader. Its goal is
stable reading first, then careful improvements such as reading statistics,
browser auto-flash, release checks, and fork/upstream cherry-picks that are worth
the risk.

Use this file as the always-on map. For task workflows, load the matching repo
skill from `.agents/skills/`; for deeper project details, read the linked
`agent-docs/` reference only when it is relevant.

## Skill Router

- `.agents/skills/cpr-firmware`: firmware code, UI activities, rendering,
  input, HAL, parsers, storage, settings, i18n, and constrained C++ changes.
- `.agents/skills/cpr-release`: version bumps, release builds, GitHub releases,
  tags, CI, GitHub Pages, auto-flash, manifests, and release documentation.
- `.agents/skills/cpr-upstream-sync`: syncing with upstream CrossPoint Reader or
  comparing/porting ideas from CrossInk, crosspet, papyrix, and other forks.
- `.agents/skills/cpr-reading-stats`: reading statistics, streaks,
  achievements, manual corrections, import/export, stats screens, and the
  browser stats editor.

## Always-On Rules

- Preserve stability over feature size. The default/`gh_release` builds target
  the ESP32-C3 (Xteink X4/X3), which has about 380 KB usable RAM and no PSRAM.
  X4 Pro distribution is withdrawn; `x4pro*` environments are compile-only
  maintainer tools and must not produce public binaries, Pages entries, or
  release assets until the withdrawal is explicitly reversed.
- X3/X4 users also depend on a working OTA/SD update path. Never infer USB
  recoverability from the model: preserve the running slot, partition table,
  bootloader, settings and recovery path. A successful build is not hardware
  validation. See `agent-docs/stability-audit-2026-09.md` for regression history
  and the X3/X4 validation matrix before another release.
- The X4 Pro build (`x4pro`, `x4pro-gh_release`, `x4pro-gh_release_rc` envs)
  runs on a dual-core ESP32-S3, so SMP rules apply: never pass NULL spinlocks to
  FreeRTOS critical sections, keep the render task pinned to one core, and do
  not assume single-core ordering between tasks.
- Prefer existing project patterns and nearby code over new abstractions.
- Inspect relevant files with `rg` before changing firmware behavior.
- User-facing device text must use the `tr()` i18n flow. Logs may be plain text.
- Avoid new heap allocation in render loops, input loops, parser hot paths, and
  repeated UI refresh paths.
- Do not edit generated outputs by hand when a generator exists.
- Do not use symlinks for `CLAUDE.md`, `AGENTS.md`, or skill files; Windows
  checkout must remain safe.
- Do not commit, tag, publish releases, or push unless the user explicitly asks.
- For releases, the browser auto-flash firmware must come from the latest
  published GitHub release asset, not from an arbitrary local build. Auto
  Flash must default to X4 and reject X4 Pro regardless of stale metadata.

## Fork Notes (Offline Wikipedia, X4 Pro)

This repository is a personal fork: CPR-vCodex plus Metske's offline Wikipedia
reader (`src/activities/wiki/`), maintained for an Xteink X4 Pro. Where these
notes differ from the upstream rules above, these notes win for this fork.

- Default branch is `main`. Upstream is `franssjz/cpr-vcodex` (`master`); the
  wiki code comes from `Metske/cpr-vcodex-wiki` (`main`), a snapshot of upstream
  commit `a4ae01a` (just after `1.6.0.38-cpr-vcodex`) published without shared
  git history. A history-join commit records that ancestry, so upstream updates
  are ordinary merges: `git fetch https://github.com/franssjz/cpr-vcodex master`
  then `git merge FETCH_HEAD`. Last merged upstream commit: `6bde1d6`
  (1.6.5.7 + 3). The 1.6.0.38-based wiki build is the merge of pull request 1.
- Expect conflicts only where a wiki hook sits beside an upstream addition
  (shortcut registry and its array size, shortcut settings and their JSON
  load/save, icon tables, the Apps/Home shortcut switches). Keep both sides.
  Then rebuild `x4pro-gh_release_rc`, run the native tests, and smoke-test the
  wiki in the simulator (`agent-docs/simulator.md`).
- Original X4/X3 (ESP32-C3): with the wiki included, current upstream no longer
  fits the 6,553,600-byte C3 slot (`gh_release` was 6,583,335 bytes at
  `6bde1d6`). This fork targets the X4 Pro; do not trim upstream features to
  make the C3 image fit without asking the owner.
- Keep the wiki isolated: wiki code lives in `src/activities/wiki/`, and hooks
  into upstream files stay as small as possible so upstream merges stay easy.
  Metske's non-wiki changes (EPUB exact page counts, image tone curves, build
  workarounds) were deliberately removed; do not re-import them with wiki
  updates. `src/activities/wiki/` keeps Metske's formatting and is excluded from
  the CI clang-format check.
- Fork-only features outside the wiki live in `src/fork/` and `lib/PerfLog/`,
  again with one-line hooks in upstream files (each marked `Fork:`):
  - Reader light controls (`src/fork/ReaderLightControls`): in a book, a
    vertical slide along the left screen edge sets brightness and along the
    right edge sets warmth. Hooks: the three reader `loop()`s. Setting
    `readerEdgeLightSlide`. The slide idea and scale follow CrossInk
    (`uxjulia/crossink`, MIT).
  - Reader side-button actions (`src/fork/ReaderSideButtons`,
    `src/fork/SideButtonSettingsActivity`): tap, double-tap and long-press
    actions for the left (`BTN_UP`) and right (`BTN_DOWN`) side buttons while
    a book page is on screen, modelled on the Home button gestures. Non-page
    actions are injected into the Home-key action channel by
    `MappedInputManager::applyReaderSideButtons()`, which `main.cpp` calls
    right after `update()`; it is deliberately not inside `update()`, which
    `test/button_press_navigation` extracts. Page actions come back through
    `ReaderUtils::detectPageTurn`. With every row on Default the module is
    inert and the stock mapping runs. Settings `side{Left,Right}{Tap,
    DoubleTap,LongPress}Action`.
  - On the carousel home a horizontal swipe that starts on the covers or the
    icon strip is never treated as the Back swipe (stock Home maps Back to
    "resume the latest book").
  - Timing log (`lib/PerfLog`, `src/fork/PerfLogWriter`): with the System
    setting `timingLog` on, input, activity enter/exit, render and panel
    refresh times are appended to `/.crosspoint/cpr-vcodex-logs/timing.log`
    while idle. Hooks: `main.cpp`, `Activity.cpp`,
    `ActivityManager::renderTaskLoop`, and the `GfxRenderer` display calls.
    Do not hook the `ActivityManager` methods that
    `test/activity_lifecycle` extracts.
  - `HomeActivity::render` clears the framebuffer only in the cover-grid
    branch (upstream clears unconditionally, which blanks the Lyra Carousel).
    The carousel's reuse of the frame already in the framebuffer is guarded by
    a framebuffer fingerprint, so a screen drawn over Home (the light panel)
    forces a reload instead of leaving its pixels behind.
  - Lyra Carousel home, touch: a horizontal swipe moves the row under the
    finger whichever row has focus. On the covers it steps one book; on the
    icon strip it pages by a full window (`MappedInputManager::wasSwipeStart`).
  - A new setting needs a row in two places: `SettingsActivity.cpp`
    (`build*Settings`, the on-device menu) and `SettingsList.cpp` (the web
    settings page), plus the field, the `JsonSettingsIO.cpp` load/save lines
    and the string in `english.yaml`. Check the on-device menu in the
    simulator; the first build of the light settings missed it.
- `ci.yml` uploads X4 Pro binaries as CI artifacts with one-day retention for
  the owner's USB-recoverable device. Do not attach X4 Pro binaries to releases,
  Pages, or the auto-flash manifest without an explicit request from the owner
  and a prominent warning about USB-locked units (upstream issue #212).
- Flash X4 Pro test builds as the app image only (offset `0x10000`,
  `partitions_x4pro.csv` unchanged). Never ask for a bootloader or partition
  table flash without explaining why and how to recover.
- The wiki reads the `.img` archive through raw SD sectors
  (`SdRawReader`) under `HalStorage::RawAccessLock`. The `.img` layout (`CPR1`
  footer, 32 KB gzip clusters) must stay compatible with `xt-zim-imager`.

## Quick Commands

```powershell
python -X utf8 -m platformio run -e default -j 1
python -X utf8 -m platformio run -e gh_release -j 1
python -X utf8 scripts/pre_release_check.py --tag <tag>
python -X utf8 scripts/sync_autoflash_firmware.py --repo franssjz/cpr-vcodex
```

If `pio` is available directly, `pio run -e default` and `pio run -e gh_release`
are equivalent. On Windows, prefer running commands from the repository root.

## MCP And Tools

No project MCP server is required for normal firmware work. Use the GitHub and
browser plugins when they are available. Add MCP only when it supplies a stable
external tool or authoritative live data that the repo itself cannot provide,
and keep tokens/secrets in user-level Codex config, never in this repository.
For the rationale and maintenance policy, read
`agent-docs/codex-agent-setup.md`.

## Compatibility

`CLAUDE.md` exists only as a short compatibility entrypoint for Claude Code.
Keep shared guidance here, task workflows in `.agents/skills/`, and long-lived
reference material in `agent-docs/`.
