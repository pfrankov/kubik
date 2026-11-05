import { randomUUID } from 'node:crypto';

// Supplemental fault coverage from the same device logs as the former scan.
// No text is retained or exported; counters survive quiet console output.
export class DeviceDiagnostics {
  instance = randomUUID();
  faults = 0;
  boots = 0;
  disconnects = 0;
  consume(text) {
    if (/speech buffer|reservation failed|alloc \d+ failed|allocation failed|read error|PANIC|Guru Meditation|panic'ed|assert failed/.test(text)) this.faults++;
    if (/ESP-ROM:esp32(?:c6|s3)/.test(text)) this.boots++;
  }
  snapshot() {
    return {ok:true, instance:this.instance, faults:this.faults, boots:this.boots, disconnects:this.disconnects};
  }
}
