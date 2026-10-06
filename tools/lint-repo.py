#!/usr/bin/env python3
"""Check source syntax and the single current documentation/scenario contract."""
import json
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parent.parent
SUFFIXES = {'.py', '.js', '.mjs', '.json', '.sh', '.yml', '.yaml'}
SCENARIO_ID = re.compile(r'^## ([A-Z]{3}-\d{2})$', re.MULTILINE)
LINK = re.compile(r'!?\[[^\]\n]*\]\(([^)]+)\)')


def source_files(root):
    names = subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], cwd=root)
    return [root / name for name in sorted(set(names.decode().split('\0'))) if name and (root / name).is_file()]


def check_syntax(path):
    if path.suffix == '.py':
        compile(path.read_text(), str(path), 'exec')
    elif path.suffix in {'.yml', '.yaml'}:
        import yaml
        yaml.load(path.read_text(), Loader=yaml.BaseLoader)
    elif path.suffix == '.json':
        json.loads(path.read_text())
    elif path.suffix in {'.js', '.mjs'}:
        subprocess.run(['node', '--check', str(path)], check=True, capture_output=True)
    elif path.suffix == '.sh':
        subprocess.run(['bash', '-n', str(path)], check=True, capture_output=True)


def check_links(path):
    errors = []
    for target in LINK.findall(path.read_text()):
        target = target.strip('<>').split('#', 1)[0]
        if not target or re.match(r'^[a-z][a-z0-9+.-]*:', target) or target.startswith('$'):
            continue
        if not (path.parent / unquote(target)).exists():
            errors.append(f'{path}: missing local link {target}')
    return errors


def check_scenarios(folder):
    errors, seen = [], set()
    for path in sorted(folder.glob('*.md')):
        text = path.read_text()
        if len(text.splitlines()) > 200:
            errors.append(f'{path}: scenario file exceeds 200 lines')
        ids = SCENARIO_ID.findall(text)
        if path.name != 'README.md' and not ids:
            errors.append(f'{path}: no scenario IDs')
        for case in re.split(r'^## ', text, flags=re.MULTILINE)[1:]:
            name = case.splitlines()[0]
            if not re.fullmatch(r'[A-Z]{3}-\d{2}', name):
                errors.append(f'{path}: invalid scenario ID {name}')
            if name in seen:
                errors.append(f'{path}: duplicate scenario ID {name}')
            seen.add(name)
            if len(re.findall(r'^Действие: .+\S', case, re.MULTILINE)) != 1:
                errors.append(f'{path}: {name} needs one user action')
            if len(re.findall(r'^Результат: .+\S', case, re.MULTILINE)) != 1:
                errors.append(f'{path}: {name} needs one observable result')
        errors.extend(check_links(path))
    if not seen:
        errors.append(f'{folder}: empty scenario contract')
    return errors


def main():
    errors = check_scenarios(ROOT / 'docs/scenarios')
    files = source_files(ROOT)
    syntax = 0
    for path in files:
        try:
            if path.suffix in SUFFIXES:
                check_syntax(path)
                syntax += 1
            current_doc = path.suffix == '.md' and ('docs' in path.parts or path.name in {'README.md', 'AGENTS.md', 'CHANGELOG.md'})
            if current_doc and 'history' not in path.parts:
                errors.extend(check_links(path))
        except (SyntaxError, ValueError, subprocess.CalledProcessError) as error:
            errors.append(f'{path.relative_to(ROOT)}: {error}')
    for error in errors:
        print(error)
    print(f'repo lint: {syntax} sources checked, {len(errors)} violations')
    raise SystemExit(bool(errors))


if __name__ == '__main__':
    main()
