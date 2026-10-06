#!/usr/bin/env python3
"""Enforce the source file and cyclomatic-complexity limits."""

from pathlib import Path

try:
    import lizard
except ImportError as error:
    raise SystemExit("Run with: uvx --from lizard==1.24.0 python tools/check-shape.py") from error


ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOTS = ("firmware/main", "firmware/sim", "openclaw-kubik/src",
                "openclaw-kubik/test", "openclaw-kubik/scripts", "tools", "deploy")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".inc", ".js", ".mjs", ".py", ".sh", ".html", ".css", ".swift"}
ANALYZED_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".js", ".mjs", ".py"}
SKIP_DIRS = {"node_modules", "build", "dist", ".venv", ".tmp"}
errors = []
file_count = 0
function_count = 0
for folder in SOURCE_ROOTS:
    for path in sorted((ROOT / folder).rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        if any(part in SKIP_DIRS for part in path.relative_to(ROOT).parts):
            continue
        file_count += 1
        relative = path.relative_to(ROOT)
        line_count = len(path.read_text(errors="replace").splitlines())
        if line_count > 500:
            errors.append(f"{relative}: {line_count} lines (max 500)")
        if path.suffix in ANALYZED_SUFFIXES:
            for function in lizard.analyze_file(str(path)).function_list:
                function_count += 1
                if function.cyclomatic_complexity > 13:
                    errors.append(
                        f"{relative}:{function.start_line} {function.name}: "
                        f"CCN {function.cyclomatic_complexity} (max 13)"
                    )
for error in errors:
    print(error)
print(f"shape: {file_count} files, {function_count} functions, {len(errors)} violations")
raise SystemExit(bool(errors))
