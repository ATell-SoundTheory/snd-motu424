const DECAY_DB_PER_SECOND = 20 / 1.7;

export interface MeterPoint {
  value: number;
  sequence: number;
}

type MeterKind = 'input' | 'output';
type Listener = () => void;

export class MeterStore {
  private inputs: (MeterPoint | null)[] = Array(96).fill(null);
  private outputs: (MeterPoint | null)[] = Array(4).fill(null);
  private bus: number | null = null;
  private inputListeners = Array.from({ length: 96 }, () => new Set<Listener>());
  private outputListeners = Array.from({ length: 4 }, () => new Set<Listener>());

  get(kind: MeterKind, index: number, bus?: number): MeterPoint | null {
    if (kind === 'input') return this.inputs[index];
    return this.bus === bus ? this.outputs[index] : null;
  }

  subscribe(kind: MeterKind, index: number, listener: Listener): () => void {
    const listeners = kind === 'input' ? this.inputListeners[index] : this.outputListeners[index];
    listeners.add(listener);
    return () => listeners.delete(listener);
  }

  publish(bank: number, bus: number, values: number[], sequence: number): void {
    const base = bank * 48;
    for (let index = 0; index < 48; index += 1) {
      this.inputs[base + index] = { value: values[index], sequence };
    }
    this.bus = bus;
    for (let index = 0; index < 4; index += 1) {
      this.outputs[index] = { value: values[48 + index], sequence };
    }
    for (let index = 0; index < 48; index += 1) {
      this.inputListeners[base + index].forEach((listener) => listener());
    }
    this.outputListeners.forEach((listeners) => listeners.forEach((listener) => listener()));
  }

  reset(): void {
    this.inputs.fill(null);
    this.outputs.fill(null);
    this.bus = null;
    this.inputListeners.forEach((listeners) => listeners.forEach((listener) => listener()));
    this.outputListeners.forEach((listeners) => listeners.forEach((listener) => listener()));
  }
}

export function advanceMeterBar(
  previousDb: number,
  currentDb: number,
  elapsedMs: number,
): number {
  const nextDb = Math.max(
    currentDb,
    previousDb - Math.max(0, elapsedMs) / 1000 * DECAY_DB_PER_SECOND,
  );
  return nextDb <= -60 ? -Infinity : nextDb;
}

export function litMeterSegments(db: number): number {
  if (db < -60) return 0;
  if (db <= -3) return Math.min(57, Math.floor(db + 60) + 1);
  return Math.min(60, Math.ceil(db + 60));
}
