import type { EvidenceStore } from './evidence-store';

type GateName = 'capture' | 'restore' | 'request' | 'initialize';

class ScenarioGate {
  private closed = false;
  private readonly waiters = new Set<() => void>();

  constructor(private readonly name: GateName) {}

  close(): void {
    this.closed = true;
  }

  open(): void {
    this.closed = false;
    for (const resolve of this.waiters) resolve();
    this.waiters.clear();
  }

  snapshot(): { readonly name: GateName; readonly closed: boolean; readonly waiterCount: number } {
    return { name: this.name, closed: this.closed, waiterCount: this.waiters.size };
  }

  async wait(evidence: EvidenceStore | undefined, detail: string, signal?: AbortSignal): Promise<void> {
    if (!this.closed) return;
    evidence?.add(`scenario-gate-held|gate=${this.name}|${detail}`);
    await new Promise<void>((resolve, reject) => {
      const complete = () => {
        signal?.removeEventListener('abort', abort);
        resolve();
      };
      const abort = () => {
        this.waiters.delete(complete);
        reject(signal?.reason ?? new Error(`Scenario ${this.name} gate was aborted.`));
      };
      this.waiters.add(complete);
      signal?.addEventListener('abort', abort, { once: true });
      if (!this.closed) {
        this.waiters.delete(complete);
        complete();
      }
    });
  }
}

const gates = new Map<GateName, ScenarioGate>(
  (['capture', 'restore', 'request', 'initialize'] as const)
    .map((name) => [name, new ScenarioGate(name)])
);
let scenarioEvidence: EvidenceStore | undefined;

export function configureScenarioGates(evidence: EvidenceStore): void {
  scenarioEvidence = evidence;
  for (const gate of gates.values()) gate.open();
}

export function closeScenarioGate(name: GateName): ReturnType<ScenarioGate['snapshot']> {
  const gate = requireGate(name);
  gate.close();
  return gate.snapshot();
}

export function openScenarioGate(name: GateName): ReturnType<ScenarioGate['snapshot']> {
  const gate = requireGate(name);
  gate.open();
  return gate.snapshot();
}

export function scenarioGateSnapshot(name: GateName): ReturnType<ScenarioGate['snapshot']> {
  return requireGate(name).snapshot();
}

export function waitForScenarioGate(
  name: GateName,
  detail: string,
  signal?: AbortSignal
): Promise<void> {
  return requireGate(name).wait(scenarioEvidence, detail, signal);
}

function requireGate(name: GateName): ScenarioGate {
  const gate = gates.get(name);
  if (gate === undefined) throw new RangeError(`Unknown scenario gate '${String(name)}'.`);
  return gate;
}

export function parseScenarioGate(value: unknown): GateName {
  if (value === 'capture' || value === 'restore' || value === 'request' || value === 'initialize') {
    return value;
  }
  throw new RangeError("Scenario gate must be 'capture', 'restore', 'request', or 'initialize'.");
}
