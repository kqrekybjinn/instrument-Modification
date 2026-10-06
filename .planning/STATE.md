# State
2026-10-04: first count reliability changes implemented.
Backup: /home/amu/backups/modi-before-count-fix-20261004/project.tar (complete working tree and Git index).
Review branch: codex/count-feedback-fix (baseline + isolated changes).
Original branch and user index preserved.
Host math regression test and final ESP-IDF v5.3 build passed. No firmware flashed. Tests validate arithmetic under the existing protocol model, not real encoder accuracy or full state-machine behavior.
Hardware validation outstanding: mileage/angle continuity, directional sign, loss handling and +/-0.5 turn stopping tolerance.
