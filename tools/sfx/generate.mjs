#!/usr/bin/env node
// Kubik sound kit: offline synthesis of every UI sound.
//
//   node tools/sfx/generate.mjs            render everything
//   node tools/sfx/generate.mjs tap pet    render only names starting with these prefixes
//
// Output: firmware/assets/sfx/<name>.pcm (s16le mono 24 kHz), manifest.json,
//         tools/sfx/out/<name>.wav previews.
// Pure JS, no dependencies. Everything is deterministic (seeded per sound name).

import { main } from './render-all.mjs';

main();
