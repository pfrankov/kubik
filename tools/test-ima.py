#!/usr/bin/env python3
"""Test the IMA ADPCM speech codec (bit-exact with the server's encoder) with ASan/UBSan."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="kubik-ima-") as tmp:
    exe = str(Path(tmp) / "test")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                    "-fsanitize=address,undefined", "firmware/sim/ima_adpcm_test.c", "-Ifirmware/main",
                    "-lm", "-o", exe], cwd=ROOT, check=True)
    wire = Path(tmp) / "mic.bin"
    subprocess.run([exe, str(wire)], cwd=ROOT, check=True)
    check = """
      import assert from 'node:assert/strict';
      import fs from 'node:fs';
      import { parseAudioFrame } from './openclaw-kubik/src/protocol.js';
      const wire = fs.readFileSync(process.argv[1]);
      assert.equal(wire.length, 4 * 2405);
      for (let i = 0; i < 4; i++) {
        const frame = wire.subarray(i * 2405, i * 2405 + 485);
        const expected = wire.subarray(i * 2405 + 485, (i + 1) * 2405);
        assert.deepEqual(parseAudioFrame(frame).pcm, expected);
        assert.equal(frame[1], i < 2 ? 9 : 10);
        if (i !== 1) assert.deepEqual([...frame.subarray(2, 5)], [0, 0, 0]);
        else assert.ok(frame.subarray(2, 5).some(byte => byte !== 0));
      }
      console.log('C microphone frames decode bit-exactly in JS across frames, turns and sessions');
    """
    subprocess.run(['node', '--input-type=module', '-e', check, str(wire)], cwd=ROOT, check=True)
