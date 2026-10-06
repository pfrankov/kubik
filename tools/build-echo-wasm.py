#!/usr/bin/env python3
"""Rebuild the pinned portable Live echo canceller. Requires Emscripten 3.1.74."""
from pathlib import Path
import argparse
import hashlib
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
REVISION = '1b28a0f61bc31162979e1f26f3981fc3637095c8'  # Xiph SpeexDSP 1.2.1
EXPORTS = ('speex_echo_state_init', 'speex_echo_cancellation', 'speex_echo_ctl',
           'speex_echo_state_destroy', 'malloc', 'free')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emcc', default='emcc')
    parser.add_argument('--check', action='store_true', help='Compare the rebuilt bytes, without writing')
    args = parser.parse_args()
    version = subprocess.check_output([args.emcc, '--version'], text=True)
    if '3.1.74' not in version.splitlines()[0]:
        raise SystemExit('Use Emscripten 3.1.74 for a reproducible binary')
    target = ROOT / 'openclaw-kubik/src/vendor/speex-aec.wasm'
    with tempfile.TemporaryDirectory(prefix='kubik-echo-') as tmp:
        source = Path(tmp) / 'speexdsp'
        subprocess.run(['git', 'clone', '--quiet', 'https://github.com/xiph/speexdsp.git', str(source)], check=True)
        subprocess.run(['git', '-C', str(source), 'checkout', '--quiet', REVISION], check=True)
        (source / 'include/speex/speexdsp_config_types.h').write_text('''#include <stdint.h>
typedef int16_t spx_int16_t;
typedef uint16_t spx_uint16_t;
typedef int32_t spx_int32_t;
typedef uint32_t spx_uint32_t;
''')
        output = Path(tmp) / 'speex-aec.wasm'
        command = [args.emcc, '-O3', '-DFLOATING_POINT', '-DUSE_SMALLFT', '-DEXPORT=',
                   f'-I{source / "include"}']
        command += [str(source / f'libspeexdsp/{name}.c') for name in ('mdf', 'fftwrap', 'smallft')]
        command += ['-s', 'STANDALONE_WASM=1', '--no-entry', '-s', 'INITIAL_MEMORY=1048576',
                    '-s', 'STACK_SIZE=65536', '-s',
                    'EXPORTED_FUNCTIONS=[' + ','.join(f'"_{name}"' for name in EXPORTS) + ']', '-o', str(output)]
        subprocess.run(command, check=True)
        if args.check:
            if output.read_bytes() != target.read_bytes():
                raise SystemExit('Speex AEC artifact differs from the pinned build')
        else:
            shutil.copyfile(output, target)
        print(f'SpeexDSP {REVISION}: {len(output.read_bytes())} bytes, SHA256 {hashlib.sha256(output.read_bytes()).hexdigest()}')


if __name__ == '__main__':
    main()
