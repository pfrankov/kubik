import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import {mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import test from 'node:test';
import {fileURLToPath} from 'node:url';
import {deviceGeometry} from './geometry.mjs';

const root = fileURLToPath(new URL('../../', import.meta.url));
function production(file, start, end) {
  const source = readFileSync(join(root, 'firmware/main', file), 'utf8');
  const first = source.indexOf(start), last = end ? source.indexOf(end, first) : source.length;
  assert.ok(first >= 0 && last > first, `${file}: source boundaries`);
  return source.slice(first, last);
}
const nativeSource = `
#include <assert.h>
#include "face_math.h"
#include "tess_internal.h"
#include "face_agent.h"
${production('face_menu.c', '#define COL_MENU_PRESSED', '// A tile popping in')}
${production('face_agent.c', '#define EXTRA', '// Geometry follows')}
${production('face_agent.c', 'static bool capability_ready(', 'static const char *capability_status(')}
${production('face_agent.c', '#define ROW_HH', '// Rows dissolve')}
${production('face_agent.c', 'static bool show_retry(', 'static void navigation(')}
${production('face_agent.c', 'static int mode_height(', 'static void mode_row(')}
${production('face_agent.c', '__attribute__((noinline)) static bool inside(')}
`;
function nativeAssertions(g) {
  const checks = [
    `assert(UI_W == ${g.width} && UI_H == ${g.height});`,
    `assert(UI_FOOTER_Y == ${g.footer} && TESS_CY == ${g.tessY});`,
    'face_t f = {.menu = {.open = true, .k = 1}}; int part;',
    `assert(face_menu_hit(&f, 72, ${g.sliderCenter}, &part) == MENU_VOLUME && part == 50);`,
    `assert(face_menu_hit(&f, 188, ${g.sliderCenter}, &part) == MENU_BRIGHT);`,
    `assert(face_menu_hit(&f, 356, ${g.settingsAgent}, &part) == MENU_AGENT);`,
    `assert(face_menu_hit(&f, 356, ${g.settingsGuide}, &part) == MENU_GUIDE);`,
    `assert(face_menu_hit(&f, 127, ${g.footer}, &part) == MENU_STATUS);`,
    `f.menu.service = true; assert(face_menu_hit(&f, 240, ${g.footer}, &part) == MENU_ACTIONS && part == 1);`,
    'f.menu.sound = true;',
    `assert(face_menu_hit(&f, 132, ${g.sliderCenter}, &part) == MENU_VOLUME);`,
    `assert(face_menu_hit(&f, 348, ${g.sliderCenter}, &part) == MENU_UI_VOLUME);`,
    'agent_menu_t m = {.open = true, .online = true, .view = AGENT_VIEW_OVERVIEW};',
    `assert(face_agent_hit(&m, 240, ${g.agentModel}) == AGENT_HIT_MODELS);`,
    `assert(face_agent_hit(&m, 240, ${g.agentVoice}) == AGENT_HIT_VOICE);`,
    'm.view = AGENT_VIEW_CLASSIC;',
    `assert(face_agent_hit(&m, 240, ${g.recognition}) == AGENT_HIT_STT);`,
    `assert(face_agent_hit(&m, 240, ${g.speech}) == AGENT_HIT_TTS);`,
    'm.view = AGENT_VIEW_GUIDE;',
    `assert(face_agent_hit(&m, 240, ${g.footer}) == AGENT_HIT_GUIDE_NEXT);`,
    `m.guide_step = 3; assert(face_agent_hit(&m, 240, ${g.footer}) == AGENT_HIT_GUIDE_DONE);`,
    'agent_model_t models[3] = {0}; m.models = models; m.count = 3; m.view = AGENT_VIEW_VOICE_MODES;',
  ];
  for (const [legacy, value] of [[141,80],[311,20],[226,50],[180,66],[300,24]])
    checks.push(`assert(face_menu_slider(${g.slider(legacy)}) == ${value});`);
  for (let i = 0; i < 3; i++) {
    checks.push(`assert(face_agent_hit(&m, 240, ${g.modeRow(i)}) == AGENT_HIT_MODEL_0 + ${i});`);
    checks.push(`assert(event_journal_row_at(${g.journalRow(i)}) == ${i});`);
  }
  checks.push('m.error[0] = 1; m.retry_kind = AGENT_REQUEST_OPTIONS;');
  for (let i = 0; i < 3; i++)
    checks.push(`assert(face_agent_hit(&m, 240, ${g.modeRow(i,true)}) == AGENT_HIT_MODEL_0 + ${i});`);
  if (g.height > 480) {
    checks.push('m.view = AGENT_VIEW_GUIDE; assert(face_agent_hit(&m, 240, 428) != AGENT_HIT_GUIDE_DONE);');
    checks.push(`assert(${g.listY(420)} <= UI_H - 32 && ${g.height - 24} < UI_H);`);
  }
  return `int main(void) {\n${checks.join('\n')}\nreturn 0; }`;
}
for (const chip of ['esp32c6', 'esp32s3']) {
  test(`${chip}: physical fixture touches hit actual native controls and retain slider values`, () => {
    const g = deviceGeometry({chip}), directory = mkdtempSync(join(tmpdir(), 'kubik-device-geometry-'));
    try {
      const source = join(directory, 'geometry.c'), executable = join(directory, 'geometry');
      writeFileSync(source, nativeSource + nativeAssertions(g));
      execFileSync(process.env.CC ?? 'cc', ['-std=c11', '-O1', '-fsanitize=address,undefined',
        '-Ifirmware/main', `-DCONFIG_IDF_TARGET_ESP32S3=${Number(chip === 'esp32s3')}`,
        source, 'firmware/main/agent_menu.c', '-lm', '-o', executable], {cwd: root, stdio: 'pipe'});
      execFileSync(executable, {stdio: 'pipe'});
    } finally { rmSync(directory, {recursive: true, force: true}); }
  });
}
test('C6 keeps every original fixture coordinate; unknown chips are rejected', () => {
  const g = deviceGeometry({chip:'esp32c6'});
  assert.deepEqual([g.height,g.footer,g.tessY,g.sliderCenter,g.settingsAgent,g.settingsGuide,
    g.agentModel,g.agentVoice,g.recognition,g.speech], [480,428,255,226,144,308,196,350,214,366]);
  for (const y of [141,311,226,180,300]) assert.equal(g.slider(y),y);
  for (const y of [150,200,220,316,350,380,384,400,420]) assert.equal(g.listY(y),y);
  assert.deepEqual([0,1,2].map(i=>g.modeRow(i)),[144,268,392]);
  assert.deepEqual([0,1,2].map(i=>g.journalRow(i)),[173,273,373]);
  for (const chip of [undefined,'esp32','esp32s2','ESP32-S3'])
    assert.throws(()=>deviceGeometry({chip}),/Unsupported device chip/);
});
