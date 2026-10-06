#!/usr/bin/env python3
"""Reject contract drift: duplicate IDs, missing results, bad links and oversized files."""
from pathlib import Path
import runpy
import tempfile

check = runpy.run_path(str(Path(__file__).with_name('lint-repo.py')))['check_scenarios']
valid = '# Category\n\n## SET-01\n\nДействие: Нажать KEY.\n\nРезультат: Начинается запись.\n'
with tempfile.TemporaryDirectory(prefix='kubik-contract-') as temp:
    folder = Path(temp)
    path = folder / 'setup.md'
    path.write_text(valid)
    assert not check(folder)
    variants = [valid.replace('Результат:', 'Состояние:'), valid + '\n' * 200,
                valid.replace('SET-01', 'bad'), valid + '\n[Broken](missing.md)\n']
    for text in variants:
        path.write_text(text)
        assert check(folder), 'invalid contract accepted'
    path.write_text(valid)
    (folder / 'other.md').write_text(valid)
    assert any('duplicate' in message for message in check(folder))
print('scenario lint: valid contract accepted, four invalid cases and duplicate rejected')
