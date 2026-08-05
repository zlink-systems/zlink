export interface ServiceWeightedSelectionCandidate<T> {
  readonly value: T;
  readonly id: string;
  readonly weight: number;
}

interface SelectionCycle<T> {
  readonly values: readonly T[];
  readonly states: readonly ReadonlyMap<string, bigint>[];
  readonly cycleStart: number;
  cursor: number;
}

/** Owns cumulative smooth-weighted selection state without exposing it to topology callers. */
export class SmoothWeightedSelection<T> {
  private readonly current = new Map<string, bigint>();
  private cycle: SelectionCycle<T>;

  constructor(
    private readonly candidateProvider: () => readonly ServiceWeightedSelectionCandidate<T>[]
  ) {
    this.cycle = emptyCycle();
    this.rebuild();
  }

  rebuild(): void {
    this.cycle = buildCycle(this.candidateProvider(), this.current);
  }

  select(accept: (value: T) => boolean = () => true): T | undefined {
    if (this.cycle.values.length === 0) return undefined;
    for (let attempts = 0; attempts < this.cycle.values.length; attempts += 1) {
      const cursor = this.cycle.cursor;
      const selected = this.cycle.values[cursor]!;
      const nextCursor = cursor + 1 < this.cycle.values.length
        ? cursor + 1
        : this.cycle.cycleStart;
      replaceState(this.current, this.cycle.states[nextCursor]!);
      this.cycle.cursor = nextCursor;
      if (accept(selected)) return selected;
    }
    return undefined;
  }
}

function buildCycle<T>(
  eligible: readonly ServiceWeightedSelectionCandidate<T>[],
  current: Map<string, bigint>
): SelectionCycle<T> {
  const candidates = eligible.map(candidate => ({
    ...candidate,
    weight: BigInt(candidate.weight)
  }));
  const total = candidates.reduce((sum, candidate) => sum + candidate.weight, 0n);
  replaceState(
    current,
    new Map(candidates.map(candidate => [
      candidate.id,
      current.get(candidate.id) ?? 0n
    ]))
  );
  if (total === 0n) return emptyCycle();

  const values: T[] = [];
  const states: ReadonlyMap<string, bigint>[] = [];
  const seen = new Map<string, number>();
  const working = new Map(current);
  for (;;) {
    const stateKey = candidates
      .map(candidate => `${candidate.id}\u0000${working.get(candidate.id) ?? 0n}`)
      .join('\u0001');
    const cycleStart = seen.get(stateKey);
    if (cycleStart !== undefined) {
      return {
        values: Object.freeze(values),
        states: Object.freeze(states),
        cycleStart,
        cursor: 0
      };
    }
    seen.set(stateKey, values.length);
    states.push(new Map(working));
    let selected = candidates[0]!;
    let selectedCurrent = working.get(selected.id)! + selected.weight;
    for (const candidate of candidates) {
      const next = working.get(candidate.id)! + candidate.weight;
      working.set(candidate.id, next);
      if (next > selectedCurrent) {
        selected = candidate;
        selectedCurrent = next;
      }
    }
    working.set(selected.id, working.get(selected.id)! - total);
    values.push(selected.value);
  }
}

function emptyCycle<T>(): SelectionCycle<T> {
  return {
    values: Object.freeze([]),
    states: Object.freeze([]),
    cycleStart: 0,
    cursor: 0
  };
}

function replaceState(
  target: Map<string, bigint>,
  source: ReadonlyMap<string, bigint>
): void {
  target.clear();
  for (const [key, value] of source) target.set(key, value);
}
