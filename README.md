# MODI Current System Features

## 1. Boot and Startup

- Power-on shows a boot screen with progress bar and the text `电机初始化中 xx%`.
- Boot settle time is 2.5 seconds, then motor driver init and UI switches to main screen.
- Default mode on startup is **free run mode**.

## 2. Main UI Layout

- Top bar:
  - `MODI` title.
  - Mode toggle button (`自由模式` / `计数模式`).
  - Forward direction button (`正转顺时` / `正转逆时`).
  - USB and TF status indicators.
- Center card:
  - Run state text (`停止` / `正转` / `反转回位` / `已暂停`).
  - Rotation count display (large font, always visible).
  - RPM display with runtime direction (`CW` / `CCW`) when running.
- Bottom buttons:
  - Speed button.
  - State button (`Start` / `Stop` / `Pause` / `Resume` depending on mode and state).

## 3. Storage Status and Logging

- USB/TF mount status is polled periodically and shown on GUI.
- TF mount retry interval is adaptive:
  - 5 seconds when mounted.
  - 30 seconds when mount failed.
- One-shot storage debug task at startup:
  - TF test file read/write.
  - USB MSC test file read/write (if mounted in timeout window).

## 4. Free Run Mode (MODE_FREE)

- **No target count.** Motor runs until manually stopped.
- State key flow:
  - Idle → start forward with smooth acceleration (S-curve ramp).
  - Running → soft stop (gradual deceleration then brake).
- Speed button:
  - Cycles through speed levels at **any time**, including while running.
  - Allows real-time speed adjustment / manual deceleration while in motion.
- Rotation count display shows elapsed rotations from start (no target denominator).
- On stop, rotation count and position state are reset to zero.

## 5. Count Mode (MODE_COUNT)

- Target count: 1–9999 rotations.
- State key flow:
  - Idle → start forward.
  - Running → pause.
  - Paused → resume in current direction.
- Speed button is mapped to abort/reverse flow (not speed step) while a run is in progress.
- Stop/Reverse key (`CMD_COUNT_ABORT`):
  - If forward running / paused / finished → start reverse return.
  - If reversing → stop reverse and reset to origin idle state.
- **Sub-rotation precision auto-stop:**
  - Position is tracked using a Q15 fixed-point value combining `mileage_rotations` and `position_raw` (1 rotation = 32768 units).
  - Forward stops when `pos_fixed_delta >= target_count × 32768`.
  - Reverse stops when `pos_fixed_now <= pos_fixed_start`.
  - Eliminates the "n circles only runs n−1" error caused by integer mileage boundary.

## 6. Smooth Speed Strategy

- Speed envelope uses an S-curve (smoothstep `3t²−2t³`) instead of linear ramps.
- **Free run:** accel-only S-curve, no automatic deceleration ramp.
- **Count mode:** symmetric asymmetric ramps:
  - Accel ramp length: `rpm² / 14400 + 2` rotations.
  - Decel ramp length: `rpm² / 14400 + 2` rotations (starts in advance to reach SLOW_RPM at target).
- Runtime slew limiter caps RPM rate-of-change each control cycle:
  - Acceleration: 120 RPM/s.
  - Deceleration: 120 RPM/s.
- `SMOOTH_SLOW_RPM = 30 RPM` — floor speed at start, end, and transitions.
- Soft stop path (manual stop / pause):
  - Multi-step deceleration at 260 RPM/s down to 8 RPM tail speed, then brake.

## 7. Speed / Count Parameter Controls

- Speed levels: 50 / 100 / 200 / 300 / 380 RPM.
- Count target range: 1 to 9999 rotations.
- Count step toggle: ×1 / ×10.
- Count target adjustment only allowed in count mode when idle at origin.
- Speed level change allowed **at any time in free run mode** (including while running).

## 8. Direction Control

- Forward direction can be toggled (`CW` / `CCW` mapping for forward motion).
- Direction switch only allowed when fully idle (not running, not paused, no active position state).

## 9. Physical Key Mapping

- Start key: `CMD_STATE_TOGGLE` in both modes.
  - Free run: toggles start / soft-stop.
  - Count mode: toggles start / pause.
- Stop key:
  - Count mode → `CMD_COUNT_ABORT` (stop/reverse flow).
  - Free run mode → `CMD_SPEED_STEP` (cycle speed level).

## 10. Safety and State Constraints

- Mode switch only allowed when fully idle (not running, not paused).
- Direction switch guarded to avoid switching during motion or paused context.
- Core control and GUI state updates are protected by mutex/semaphore logic.
