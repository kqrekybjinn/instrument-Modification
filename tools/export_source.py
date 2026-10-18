#!/usr/bin/env python3
"""Export traceable first-party source text for subsequent document layout."""
from pathlib import Path
import csv
import hashlib

root = Path(__file__).resolve().parents[1]
out = root / "dist/copyright"
out.mkdir(parents=True, exist_ok=True)
files = [root / "main/app_main.cpp"]
for folder in ["components/app/include", "components/app/src", "components/bsp/include", "components/bsp/src"]:
    files.extend(p for p in sorted((root / folder).iterdir())
                 if p.suffix in {".h", ".hpp", ".c", ".cpp"} and not p.name.startswith("lv_font_"))
total = 0
with (out / "source-full.txt").open("w", encoding="utf-8") as text, (out / "source-manifest.csv").open("w", encoding="utf-8", newline="") as manifest:
    writer = csv.writer(manifest)
    writer.writerow(["path", "lines", "sha256"])
    for path in files:
        data = path.read_bytes()
        content = data.decode("utf-8")
        count = len(content.splitlines())
        total += count
        writer.writerow([path.relative_to(root).as_posix(), count, hashlib.sha256(data).hexdigest()])
        text.write("// FILE: " + path.relative_to(root).as_posix() + "\n")
        text.write(content)
        if not content.endswith("\n"):
            text.write("\n")
print(f"Exported {len(files)} files, {total} source lines to {out}")
