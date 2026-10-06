# Key Control Workspace

## Current understanding
- Project type: ESP32-S3 + ESP-IDF motor controller (`main/app_main.cpp` starts `AppController`).
- Key scan task: `components/app/src/app_input_service.cpp`.
- Key pins: `START=GPIO12`, `STOP=GPIO15` in `components/bsp/include/bsp_pinmap.h`.
- Command bridge: `components/app/src/app_controller.cpp` maps GUI events to control commands.
- Control state machine: `components/app/src/app_control_service.cpp`.

## Existing physical key behavior
- `START` press edge → `GUI_EVENT_STATE_TOGGLE`.
- `STOP` press edge:
  - Count mode: send `CMD_COUNT_ABORT` (brake + return logic).
  - Timer mode: send `GUI_EVENT_SPEED_STEP`.

## Files likely to change for key-control updates
- `components/app/src/app_input_service.cpp`
- `components/app/include/app_types.hpp` (if new command enum or scan timing changes)
- `components/app/src/app_control_service.cpp` (if state-machine semantics change)
- `components/app/src/app_controller.cpp` / `components/app/include/gui_app.h` (if event routing changes)

## Next step
- Wait for your exact key-control requirements, then patch with minimal changes and verify build.
