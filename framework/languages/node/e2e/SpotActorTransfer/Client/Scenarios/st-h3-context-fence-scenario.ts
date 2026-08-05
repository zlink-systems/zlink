// ST-H3: Context identity 시나리오를 검증한다.
import {
  SpotActorTransferNames,
  actorNode,
  getEvidence,
  require,
  runRemoteTransfer,
  unique
} from '../Support/scenario-support';

export async function runStH3(): Promise<void> {
  const actorId = unique('actor-h3');
  await runRemoteTransfer(
    'ST-H3',
    actorId,
    SpotActorTransferNames.actorTypeStateful,
    203,
    true
  );
  const evidence = [
    ...await getEvidence(actorNode('actor-a')),
    ...await getEvidence(actorNode('actor-b'))
  ].filter(entry =>
    entry.scenario === 'ST-H3'
    && entry.actorId === actorId
    && entry.kind === 'context_identity'
  );
  require(
    evidence.length === 2 && evidence.every(entry => entry.value === 'same'),
    'ST-H3 source and target factory did not preserve Context identity.'
  );
}
