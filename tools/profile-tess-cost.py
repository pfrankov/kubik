#!/usr/bin/env python3
"""Capture one bounded macOS Time Profiler trace of the Tess native test."""
from collections import Counter
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parent.parent
SOURCES = [
    "firmware/sim/tess_sound_test.c",
    "firmware/sim/tess_sound_rules_test.c",
    "firmware/sim/tess_sound_cost_test.c",
    "firmware/sim/tess_sound_profiles_test.c",
    "firmware/main/tess_sound.c",
    "firmware/main/tess_sound_compose.c",
    "firmware/main/tess_sound_rules.c",
]
FLAGS = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra",
         "-fsanitize=address,undefined"]
SYMBOLS = ("check_cost", "tess_sound_mix")
PROFILE_SCHEMAS = ("time-profile", "time-sample")
MAX_SNIPPETS = 10
MAX_SNIPPET_CHARS = 5000
LAUNCHER_SOURCE = r'''#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fputs("profile launcher: expected stderr-log and test-executable paths\n", stderr);
        return 126;
    }
    int fd = open(argv[1], O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        fprintf(stderr, "profile launcher: open stderr log failed: %s\n", strerror(errno));
        return 126;
    }
    if (dup2(fd, STDERR_FILENO) < 0) {
        int error = errno;
        if (fd != STDERR_FILENO) close(fd);
        fprintf(stderr, "profile launcher: redirect stderr failed: %s\n", strerror(error));
        return 126;
    }
    if (fd != STDERR_FILENO && close(fd) < 0) {
        fprintf(stderr, "profile launcher: close stderr log failed: %s\n", strerror(errno));
        return 126;
    }
    execl(argv[2], argv[2], (char *)NULL);
    fprintf(stderr, "profile launcher: exec test failed: %s\n", strerror(errno));
    return 127;
}
'''


class ProfileUnavailable(RuntimeError):
    pass


def save_stream(path, value):
    if isinstance(value, bytes):
        value = value.decode("utf-8", errors="replace")
    path.write_text(value or "", errors="replace")


def run_capture(label, command, timeout, log_dir, echo=False):
    print(f"== {label} ==", flush=True)
    print("$ " + " ".join(str(part) for part in command), flush=True)
    try:
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired as error:
        stdout, stderr = error.stdout or "", error.stderr or ""
        save_stream(log_dir / f"{label}.stdout.log", stdout)
        save_stream(log_dir / f"{label}.stderr.log", stderr)
        print(f"Timed out after {timeout}s; partial output saved.", flush=True)
        if echo:
            print(stdout, end="", flush=True)
            print(stderr, end="", file=sys.stderr, flush=True)
        return None, stdout, stderr
    save_stream(log_dir / f"{label}.stdout.log", result.stdout)
    save_stream(log_dir / f"{label}.stderr.log", result.stderr)
    print(f"exit status: {result.returncode}; output saved under {log_dir}", flush=True)
    if echo:
        print(result.stdout, end="", flush=True)
        print(result.stderr, end="", file=sys.stderr, flush=True)
    return result.returncode, result.stdout, result.stderr


def require_success(label, result):
    if result[0] != 0:
        raise ProfileUnavailable(f"{label} failed or timed out; see saved stdout/stderr")
    return result[1] + result[2]


def check_options(label, help_text, required):
    missing = [option for option in required if option not in help_text]
    if missing:
        raise ProfileUnavailable(f"{label} lacks required options: {', '.join(missing)}")


def probe_xctrace(log_dir):
    if shutil.which("xcrun") is None:
        raise ProfileUnavailable("xcrun is unavailable; Time Profiler requires macOS with Xcode")
    find = run_capture("find-xctrace", ["xcrun", "--find", "xctrace"], 15, log_dir)
    xctrace_path = require_success("xcrun --find xctrace", find).strip()
    if not xctrace_path:
        raise ProfileUnavailable("xcrun did not return an xctrace path")
    templates = require_success("xctrace list templates",
                                run_capture("list-templates", ["xcrun", "xctrace", "list", "templates"],
                                            20, log_dir))
    if not re.search(r"(?m)^\s*(?:[-*•]\s*)?Time Profiler(?:\s|$)", templates):
        raise ProfileUnavailable("the installed xctrace does not list the Time Profiler template")
    record_help = require_success("xctrace help record",
                                  run_capture("record-help", ["xcrun", "xctrace", "help", "record"],
                                              15, log_dir))
    export_help = require_success("xctrace help export",
                                  run_capture("export-help", ["xcrun", "xctrace", "help", "export"],
                                              15, log_dir))
    check_options("xctrace record", record_help,
                  ("--template", "--no-prompt", "--time-limit", "--output", "--launch",
                   "--target-stdout"))
    check_options("xctrace export", export_help, ("--input", "--toc", "--xpath", "--output"))
    print(f"xctrace available: {xctrace_path}; Time Profiler and required flags found.", flush=True)


def prepare_output_dir():
    runner_temp = os.environ.get("RUNNER_TEMP")
    if not runner_temp:
        raise ProfileUnavailable("RUNNER_TEMP is required to preserve the binary and trace artifacts")
    output = Path(runner_temp) / "kubik-sound-profile"
    output.mkdir(parents=True, exist_ok=True)
    trace = output / "tess-sound.trace"
    if trace.exists() or (output / "record-attempted").exists():
        raise ProfileUnavailable(f"refusing to repeat or replace the one-shot capture in {output}")
    return output, trace


def compile_test(output, log_dir):
    executable = output / "test-tess-cost"
    command = [os.environ.get("CC", "cc"), *FLAGS, *SOURCES, "-lm", "-o", str(executable)]
    result = run_capture("compile", command, 90, log_dir, echo=True)
    require_success("native sound test compile", result)
    return executable


def compile_launcher(output, log_dir):
    source = output / "profile-launcher.c"
    executable = output / "profile-launcher"
    source.write_text(LAUNCHER_SOURCE, encoding="utf-8", errors="replace")
    command = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
               "-Wall", "-Wextra", "-Werror", str(source), "-o", str(executable)]
    result = run_capture("compile-launcher", command, 30, log_dir, echo=True)
    require_success("stderr exec-launcher compile", result)
    return executable


def print_target_logs(output):
    stdout_path = output / "target-stdout.log"
    stderr_path = output / "target-stderr.log"
    contents = {}
    for name, path in (("stdout", stdout_path), ("stderr", stderr_path)):
        print(f"== target {name} ({path}) ==", flush=True)
        if not path.is_file():
            print("MISSING", flush=True)
            contents[name] = None
            continue
        contents[name] = path.read_text(errors="replace")
        print(contents[name], end="", flush=True)
    stdout = contents["stdout"] or ""
    stderr = contents["stderr"] or ""
    cost_markers = "synth: " in stderr and "synth: worst input play " in stderr
    success_marker = "Tess sound family: every sound and cue clean" in stdout
    if contents["stdout"] is None or contents["stderr"] is None:
        print("INCONCLUSIVE: target log file missing; launch or output capture may have failed.")
    elif re.search(r"assertion failed|assertion .* failed|ERROR: AddressSanitizer|runtime error:",
                   stderr, re.IGNORECASE):
        print("Harness failure/abort diagnostic observed; this profiling job is not an acceptance pass.")
        if not cost_markers:
            print("INCONCLUSIVE: expected sound-cost diagnostics are absent from target stderr.")
    elif not cost_markers:
        print("INCONCLUSIVE: expected sound-cost diagnostics are absent from target stderr.")
    elif success_marker:
        print("Harness emitted its final success marker; recorder status is not used as the test verdict.")
    else:
        print("INCONCLUSIVE: cost diagnostics exist but the final harness marker is absent.")


def record_profile(launcher, executable, trace, output, log_dir):
    marker = trace.parent / "record-attempted"
    try:
        with marker.open("x", encoding="utf-8") as handle:
            handle.write("one profiling attempt started\n")
    except FileExistsError as error:
        raise ProfileUnavailable("refusing to start a second profiling attempt") from error
    target_stdout = output / "target-stdout.log"
    target_stderr = output / "target-stderr.log"
    command = ["xcrun", "xctrace", "record", "--template", "Time Profiler", "--no-prompt",
               "--time-limit", "3m", "--target-stdout", str(target_stdout), "--output", str(trace),
               "--launch", "--", str(launcher), str(target_stderr), str(executable)]
    result = run_capture("record", command, 210, log_dir, echo=True)
    print(f"Recorder exit status: {result[0]}; this is not an independent harness-pass result.", flush=True)
    print_target_logs(output)
    if result[0] is None:
        raise ProfileUnavailable("xctrace record exceeded its outer timeout; trace may be incomplete")
    if not trace.exists():
        raise ProfileUnavailable("xctrace record returned without creating a trace")
    return result[0]


def table_description(element):
    details = [f"{key}={value}" for key, value in sorted(element.attrib.items())]
    details.extend(text.strip() for text in element.itertext() if text.strip())
    return " ".join(details)[:500]


def local_name(element):
    return element.tag.split("}")[-1]


def named_children(element, name):
    return [child for child in element if local_name(child) == name]


def print_toc_tables(root):
    for element in root.iter():
        if local_name(element) == "table":
            print(f"TOC table: {table_description(element)}")


def time_profile_tables(root, run):
    selected = []
    seen = set()
    for data in named_children(run, "data"):
        for table in named_children(data, "table"):
            schema = table.get("schema")
            if schema in PROFILE_SCHEMAS and schema not in seen:
                run_number = run.get("number", "1")
                xpath = f"/{local_name(root)}/run[@number='{run_number}']/data/table[@schema='{schema}']"
                selected.append((schema, xpath))
                seen.add(schema)
    return selected


def select_profile_tables(toc_path):
    try:
        root = ET.parse(toc_path).getroot()
    except ET.ParseError as error:
        raise ProfileUnavailable(f"xctrace table-of-contents XML is invalid: {error}") from error
    runs = named_children(root, "run")
    print_toc_tables(root)
    if not runs:
        raise ProfileUnavailable("the trace TOC contains no run element")
    return time_profile_tables(root, runs[0])


def concise_xml(element, limit=MAX_SNIPPET_CHARS):
    value = ET.tostring(element, encoding="unicode")
    if len(value) > limit:
        return value[:limit] + " ... [snippet truncated]"
    return value


def element_label(element):
    values = [element.tag, *element.attrib.values()]
    if element.text and element.text.strip():
        values.append(element.text.strip())
    return " ".join(values).lower()


SAMPLE_TAGS = {"row", "sample", "stack", "backtrace", "callstack"}


def sample_container(element, parents):
    current = element
    for _ in range(12):
        if local_name(current).lower() in SAMPLE_TAGS:
            return current
        if current not in parents:
            return None
        current = parents[current]
    return None


def find_symbol_elements(root):
    counts = {symbol: 0 for symbol in SYMBOLS}
    hits, symbol_ids = [], {}
    for element in root.iter():
        label = element_label(element)
        matched = False
        for symbol in SYMBOLS:
            if symbol in label:
                counts[symbol] += 1
                matched = True
        if matched:
            hits.append(element)
            if element.get("id"):
                symbol_ids[element.get("id")] = element
    references = []
    for element in root.iter():
        if any(element.get(key) in symbol_ids for key in ("ref", "idref")):
            references.append(element)
    return counts, hits, symbol_ids, references


def collect_containers(elements, parents):
    containers = []
    for element in elements:
        container = sample_container(element, parents)
        if container is not None and container not in containers:
            containers.append(container)
    return containers


def print_reference_targets(container, id_elements):
    refs = []
    for element in container.iter():
        for key in ("ref", "idref"):
            reference = element.get(key)
            if reference and reference in id_elements and reference not in refs:
                refs.append(reference)
    for reference in refs[:8]:
        print(f"resolved {reference}: {concise_xml(id_elements[reference], 1200)}")
    if len(refs) > 8:
        print(f"additional references omitted: {len(refs) - 8}")


def print_profile_samples(containers, id_elements):
    count = min(len(containers), MAX_SNIPPETS)
    print(f"Showing {count} of {len(containers)} unique XML contexts as bounded stack/sample snippets.")
    for index, container in enumerate(containers[:MAX_SNIPPETS], 1):
        print(f"--- stack/sample {index}: {element_label(container)[:220]} ---")
        print(concise_xml(container))
        print_reference_targets(container, id_elements)
    if len(containers) > MAX_SNIPPETS:
        print(f"additional containing snippets omitted: {len(containers) - MAX_SNIPPETS}")


def show_profile(profile_path):
    try:
        root = ET.parse(profile_path).getroot()
    except ET.ParseError as error:
        print(f"Profile XML parse failed: {error}; saved XML is available for manual inspection.")
        return
    parents = {child: parent for parent in root.iter() for child in parent}
    id_elements = {element.get("id"): element for element in root.iter() if element.get("id")}
    tags = Counter(local_name(element) for element in root.iter())
    print(f"Profile XML structure: {len(tags)} tag names, {sum(tags.values())} elements")
    print("Most common tags: " + ", ".join(f"{name}={count}" for name, count in tags.most_common(20)))
    counts, hits, symbol_ids, references = find_symbol_elements(root)
    print("Exact symbol-bearing elements: " + ", ".join(
        f"{symbol}={counts[symbol]}" for symbol in SYMBOLS))
    print(f"Symbol IDs: {len(symbol_ids)}; XML elements referencing them: {len(references)}")
    containers = collect_containers((*hits, *references), parents)
    if not containers:
        print("INCONCLUSIVE: no exact check_cost/tess_sound_mix symbols found in exported profile XML.")
        return
    print_profile_samples(containers, id_elements)
    print("Interpretation limit: sampling can show observed stacks, but cannot attribute an individual 1–2 ms mixer call.")


def export_profile(trace, output, log_dir):
    toc_path = output / "trace-toc.xml"
    result = run_capture("export-toc", ["xcrun", "xctrace", "export", "--input", str(trace), "--toc"],
                         45, log_dir, echo=True)
    if result[0] != 0:
        require_success("xctrace export --toc", result)
    toc = result[1]
    toc_path.write_text(toc, encoding="utf-8", errors="replace")
    print(f"Trace of contents saved: {toc_path}")
    tables = select_profile_tables(toc_path)
    if not tables:
        print("INCONCLUSIVE: TOC has no data table with schema 'time-profile' or 'time-sample'; "
              "the trace and TOC remain saved for inspection.")
        return
    for schema, xpath in tables:
        print(f"Selected Time Profiler table schema={schema}, XPath: {xpath}")
        profile_path = output / f"{schema}.xml"
        label = f"export-{schema}"
        result = run_capture(label, ["xcrun", "xctrace", "export", "--input", str(trace),
                                     "--xpath", xpath, "--output", str(profile_path)],
                             90, log_dir, echo=True)
        require_success(f"xctrace export schema={schema}", result)
        if not profile_path.exists():
            raise ProfileUnavailable(f"xctrace reported success without writing {profile_path}")
        print(f"Time Profiler XML schema={schema} saved: {profile_path} ({profile_path.stat().st_size} bytes)")
        show_profile(profile_path)


def main():
    try:
        output, trace = prepare_output_dir()
        log_dir = output / "logs"
        log_dir.mkdir(exist_ok=True)
        probe_xctrace(log_dir)
        executable = compile_test(output, log_dir)
        launcher = compile_launcher(output, log_dir)
        record_status = record_profile(launcher, executable, trace, output, log_dir)
        export_profile(trace, output, log_dir)
        print("Profile capture/export finished. This is diagnostic evidence, not acceptance; "
              "the sound budget assertion must be assessed from the recorded harness output.")
        return 0 if record_status == 0 else 1
    except (ProfileUnavailable, OSError) as error:
        print(f"UNAVAILABLE/ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
