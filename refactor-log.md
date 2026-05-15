# Refactor Log

## 2026-05-14

- scope of refactor:
  - maintainability and remediation pass after QA findings
- key changes:
  - created architecture and design docs
  - tightened public vs protected config behavior
  - added local UI auth/session flow for protected actions
  - improved event-log chronology metadata with `seq`, `boot_id`,
    and `ts_basis`
  - removed unhealthy secondary central default from shipped config validation
  - added API and UI-auth regression scripts
  - fixed a follow-up auth-header merge bug in the browser helper during live retest
- architectural decisions:
  - public local config reads stay available, but central identity/secrets
    are redacted
  - full secret-bearing config export stays on a protected endpoint
  - current-tab session storage is the default local UI auth persistence model
- files impacted:
  - `src/web_server_manager.cpp`
  - `data/index.html`
  - `data/style.css`
  - `data/app.js`
  - `src/config_manager.cpp`
  - `include/types.h`
  - `src/event_log.cpp`
  - `include/event_log.h`
  - `include/firmware_version.h`
- risks:
  - fallback UI and LittleFS UI must stay aligned
  - live-device verification still required for OTA-served assets
- remaining issues:
  - full route-level 405 coverage is still partial
  - button/recovery destructive paths still need hardware-assisted retest
- next recommended actions:
  - finish regression automation
  - validate on `.48`
  - then mirror the artifact and notes to shared locations

## 2026-05-14 Deep Retest Follow-up

- scope of refactor:
  - release-hardening retest on the live `0.1.21-dev-central-safe` artifact
- key changes:
  - reran the API regression sweep on the live bench device
  - reran the deep API retest on the live bench device
  - ran a 3-cycle OTA stress pass on `.48`
  - verified the actual device-served UI end to end for unlock, relay control,
    config save, clear-lock, and failed unlock behavior
  - documented and fixed a live-artifact mismatch where the served `app.js`
    lagged the repo-side auth-header merge fix until the `0.1.21` rebuild
- architectural decisions:
  - actual device-served UI assets are the authoritative release surface for
    UI/auth validation; repo-source and proxy checks are supporting evidence
- files impacted:
  - `bug-log.md`
  - `test-plan.md`
  - `qa-notes.md`
  - `architecture.md`
  - `design.md`
- risks:
  - button/recovery destructive paths still need hardware-assisted retest
  - power-sample upload path still lacks a live receiving-hub validation pass
- remaining issues:
  - no confirmed regression remained in the remediated bug set after the 0.1.21 pass
  - OTA recovery-mode quirk was not reproduced, but should still be watched
- next recommended actions:
  - mirror the `0.1.21` artifact and retest notes to shared locations
  - do the hardware button/recovery retest next
