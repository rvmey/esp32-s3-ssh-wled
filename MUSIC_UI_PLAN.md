# Music UI Recovery Plan (Core2 Picture Frame)

Date: 2026-05-25
Scope: Restore a reliable on-screen Now Playing UI for MP3 playback and command/touch control.

## 1) Current State Snapshot

- MP3 decode/playback pipeline exists in main/picture_frame.c with a dedicated task.
- Now Playing render exists (mp3_render_now_playing) and is scheduled via s_mp3_ui_pending.
- TriggerCMD commands for play/stop/next/previous/forward/reverse/volume/shuffle/repeat are present.
- Touch hit zones for playback controls exist in pf_touch_handler.
- Main loop only renders MP3 UI when all of the below are true:
  - s_mp3_ui_pending
  - s_mp3.active
  - s_mp3_ui_override_allowed
  - no pending JPEG draw/redraw
- Known historical issue: calling UI draw from decode path causes audio stutter; UI updates must stay in main loop.

## 2) Most Likely Failure Points

1. UI override flag gets disabled and not re-enabled in some command/display paths.
2. JPEG pipeline keeps pre-empting MP3 UI refreshes.
3. UI refresh events are being dropped instead of deferred when overlay is blocked.
4. Command parity mismatch (main code supports repeattrack/repeatplaylist, but JSON command file may not).
5. Touch zones not mapping correctly under orientation changes for some screens.

## 3) Recovery Goals

1. MP3 Now Playing screen appears within 500 ms after any MP3 control action.
2. UI remains responsive while audio plays for at least 30 minutes.
3. JPEG mode and MP3 mode can switch cleanly without requiring reboot.
4. Touch controls and TriggerCMD commands produce identical behavior/state updates.
5. No regressions in MP3 continuity (no added stutter, no BT reconnect regressions).

## 4) Implementation Plan

### Phase A: Reproduce and Baseline

1. Reproduce on Core2 with SD card and one JPEG command followed by MP3 commands.
2. Capture logs around:
   - command dispatch branch for play/stop/next/forward/etc.
   - mp3_request_ui_refresh calls
   - main loop UI gate conditions
3. Record baseline expected vs actual for:
   - remote command response
   - on-screen state changes
   - touch interaction response

Exit criteria:
- A single concise log timeline identifies exactly where UI updates stop.

### Phase B: Make UI Scheduling Deterministic

1. Keep all drawing in main loop only (no decode-task drawing).
2. Replace drop behavior for blocked MP3 UI updates with deferred behavior:
   - if blocked by JPEG pending state, keep pending flag set and retry next loop.
   - if blocked by override false while MP3 is active, add explicit transition rules for re-enable.
3. Centralize MP3 UI eligibility into one helper to reduce branch drift.
4. Add rate limit for periodic refresh but always allow immediate refresh on control events.

Exit criteria:
- Every control action causes one immediate UI refresh request and eventual render.

### Phase C: Command and Touch Parity

1. Verify command list parity between:
   - runtime command table in main/picture_frame.c
   - main/picture_frame_commands.json
2. Add missing entries in JSON if needed (repeattrack/repeatplaylist are common gaps).
3. Validate touch mapping in portrait and landscape:
   - play/pause row
   - prev/next row
   - seek row
   - volume row

Exit criteria:
- TriggerCMD command set and touch controls drive the same final state.

### Phase D: Persistence and UX Rules

1. Decide and enforce precedence model:
   - JPEG explicit command owns screen until a music control event reclaims it, or
   - MP3 active playback can reclaim after timeout.
2. Ensure save/restore behavior does not trap UI in non-music state while MP3 is active.
3. Keep status drawing readable and avoid conflicting colors.

Exit criteria:
- Reboot + restore does not break MP3 UI re-entry.

### Phase E: Stability and Regression Validation

1. 30-minute MP3 playback with periodic command/touch interactions.
2. Bluetooth pair/disconnect/reconnect while MP3 active.
3. Verify telemetry trends still healthy (decode/output timing, underflows, ring behavior).
4. Verify no lockups in screen redraw path.

Exit criteria:
- No audio regression and no UI dead state across the test matrix.

## 5) File-Level Worklist

- main/picture_frame.c
  - Main loop UI gate logic
  - mp3_request_ui_refresh call paths
  - mp3_render_now_playing scheduling
  - touch handler behavior under orientation
  - command branch parity and override rules

- main/picture_frame_commands.json
  - Ensure public command definitions match runtime behavior and supported toggles

- Optional test notes file
  - Capture reproducible scenarios and pass/fail matrix for future regressions

## 6) Risk Controls

1. Do not move screen drawing into MP3 decode task.
2. Keep Core2 audio sample-rate switch behavior unchanged unless directly required.
3. Preserve BT handoff protections and writer-task safety checks.
4. Keep changes focused to UI state machine and command parity first.

## 7) Done Definition

Feature is complete when all are true:

1. Music UI consistently appears and updates during active MP3 playback.
2. All transport paths (TriggerCMD and touch) control the same MP3 state.
3. JPEG and MP3 transitions are predictable and recoverable.
4. Long-run audio playback remains stable with no new stutter/regression.
5. Core2 build passes and runtime test checklist passes.

## 8) Release Procedure Reminder

If firmware source files are modified to implement this plan:

1. Bump patch version in required version files.
2. Build the active variant only (Core2 if this remains Core2-focused work).
3. Update installer pinning/versioned manifest entries when relevant.
4. Commit and push with versioned release message.
