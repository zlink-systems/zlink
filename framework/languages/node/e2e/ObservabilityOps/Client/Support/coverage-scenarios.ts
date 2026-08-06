import { ObservabilityOpsNames, createActor, nodeA, probeActor, require, unique } from './scenario-support.js';

/** Exercises the role-backed public actor probe and application evidence. */
export async function runObservabilityCoverage(scenario: string): Promise<void> {
  const actorId = unique(scenario.toLowerCase());
  const marker = unique('marker');
  const actor = await createActor(nodeA, actorId, ObservabilityOpsNames.actorTypeStateful, 1);
  require(actor.actorId === actorId, scenario + ' actor creation returned the wrong identity.');
  const reply = await probeActor(nodeA, actorId, scenario, marker);
  require(reply.actorId === actorId && reply.marker === marker, scenario + ' actor probe reply mismatch.');
  console.log('scenario ' + scenario + ' passed');
}

