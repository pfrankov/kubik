// Logical touch coordinates follow the installed chip, independently of panel pixels.
export function deviceGeometry(info) {
  if (!['esp32c6', 'esp32s3'].includes(info.chip)) throw Error(`Unsupported device chip: ${info.chip}`);
  const height = info.chip === 'esp32s3' ? 584 : 480;
  const extra = height - 480;
  const slider = y => Math.round(84 + (y - 84) * (height - 196) / 284);
  return Object.freeze({
    width: 480, height, footer: height - 52, tessY: height === 480 ? 255 : height / 2,
    slider, sliderCenter: height / 2 - 14,
    settingsAgent: 144 + extra / 4, settingsGuide: 308 + 3 * extra / 4,
    agentModel: 196 + extra / 4, agentVoice: 350 + 3 * extra / 4,
    recognition: 214 + extra / 4, speech: 366 + 3 * extra / 4,
    modeRow(index, retry = false) {
      const size = (retry ? 92 : 112) + Math.trunc(extra / 3);
      return 88 + Math.trunc(size / 2) + index * (size + 12);
    },
    // Preserve the original test's clearance from the model-list viewport edges.
    listY: y => y > 240 ? y + extra : y,
    journalRow: index => 128 + Math.trunc(index * (height - 180) / 3) + Math.round(45 * (height - 180) / 300),
  });
}
