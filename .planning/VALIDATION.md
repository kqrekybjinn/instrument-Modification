# Validation and next hardware checks
- Full backup: /home/amu/backups/modi-before-count-fix-20261004/project.tar; SHA256SUMS alongside.
- Host test: g++ -std=c++17 -Wall -Wextra -Werror -Icomponents/app/include tests/position_math_test.cpp -o /tmp/modi-position-test && /tmp/modi-position-test
- ESP-IDF v5.3: idf.py build succeeded; generated build/modi.bin.
- No flash, no motor movement performed.
- Hardware sequence: capture raw 0x75 frames at rest, then cross a turn boundary in both directions. Verify signed mileage and angle are coherent before relying on Q15 composite position.
- Test arbitrary starting angles, targets 1/10/100 turns, CW/CCW, pause/resume and return.
- Confirm final measured error <=0.5 turn; no automatic reverse correction.
- Disconnect feedback during a run: zero-speed request after 300ms without valid feedback; resume refused while feedback invalid. Actual stopping cannot be guaranteed if the physical command link is broken.
- Final stop capture: 3 valid stable position samples (change <=32 Q15 units each), maximum wait 1s. This is a software settling heuristic, not a motor accuracy specification.
- Existing deceleration constants unchanged; tune only after measurements.
- Known scope: logs add Q15 displacement and feedback-valid flag, but final stopped result is displayed rather than persisted as a separate log event. Log retention is deferred.

2026-10-06 更新：连续静止确认、失联停止/恢复门控、位置跳变保护已删除，以 02-02-SUMMARY.md 为准。
