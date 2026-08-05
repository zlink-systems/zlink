// ST-H2: Completion outcome 시나리오를 검증한다.
import {
  SpotActorTransferNames,
  actorNode,
  createActor,
  createSpot,
  getEvidence,
  joinActor,
  nodeA,
  nodeB,
  options,
  probeActor,
  require,
  waitActorRef,
  waitEvidence
} from '../Support/scenario-support';

const scenario = 'ST-H2';

export async function prepareStH2TargetRestart(): Promise<void> {
  const fixtureId = requireFixtureId();
  const { actorId, source } = await createActorOutsideRecoveryNode(fixtureId);
  const sourceNode = actorNode(source.nodeRid);
  const targetSpot = await createRecoverySpotOutside(source.nodeRid, fixtureId);
  const targetNode = actorNode(targetSpot.nodeRid);
  const join = await joinActor(sourceNode, actorId, {
    scenario,
    targetSpotId: targetSpot.spotId,
    transferId: `h2-transfer-${fixtureId}`
  });
  require(join.accepted, 'ST-H2 deferred Join submission failed.');
  const evidence = await waitEvidence(targetNode, [
    `${scenario}|${actorId}|deferred_completion_staged|`,
    `${scenario}|${actorId}|join_completion_started|`
  ]);
  const started = evidence.find(entry =>
    entry.scenario === scenario
    && entry.actorId === actorId
    && entry.kind === 'join_completion_started'
  );
  require(started !== undefined, 'ST-H2 completion operation evidence is missing.');
  require(
    /^\d+:\d+$/.test(started.value) && started.value !== '0:0',
    'ST-H2 completion operation ID must be a non-zero 128-bit pair.'
  );
  const publishedAuthority = await waitActorRef(
    sourceNode,
    actorId,
    targetSpot.nodeRid
  );
  console.log([
    'st-h2-restart-fixture',
    `actorId=${actorId}`,
    `targetNodeRid=${targetSpot.nodeRid}`,
    `objectGeneration=${publishedAuthority.objectGeneration}`,
    `operationId=${started.value}`
  ].join(' '));
}

export async function verifyStH2TargetRestart(): Promise<void> {
  requireFixtureId();
  const actorId = requireOption(options.actorId, 'actorId');
  const targetNodeRid = requireOption(options.targetNodeRid, 'targetNodeRid');
  const objectGeneration = requireOption(
    options.expectedObjectGeneration,
    'expectedObjectGeneration'
  );
  const operationId = requireOption(options.expectedOperationId, 'expectedOperationId');
  const targetNode = actorNode(targetNodeRid);
  const authority = await waitActorRef(nodeA, actorId, targetNodeRid);
  require(
    authority.objectGeneration === objectGeneration,
    'ST-H2 restart changed the Actor object generation.'
  );
  const entries = await waitEvidence(targetNode, [
    `${scenario}|${actorId}|join_completion|accepted|${operationId}`
  ], 40_000);
  const completions = entries.filter(entry =>
    entry.scenario === scenario
    && entry.actorId === actorId
    && entry.kind === 'join_completion'
  );
  require(completions.length === 1, 'ST-H2 restart delivered Join completion more than once.');
  const probe = await probeActor(targetNode, actorId, scenario, 'after-target-restart');
  require(
    probe.nodeRid === targetNodeRid && probe.stateVersion === 121,
    'ST-H2 restart did not restore the target Actor state.'
  );
  const afterProbe = await getEvidence(targetNode);
  require(
    afterProbe.some(entry =>
      entry.scenario === scenario
      && entry.actorId === actorId
      && entry.kind === 'packet_handler'
      && entry.value === 'after-target-restart'
    ),
    'ST-H2 target did not admit a packet after recovery.'
  );
}

async function createRecoverySpotOutside(
  sourceNodeRid: string,
  fixtureId: string
): Promise<{ spotId: string; nodeRid: string }> {
  for (let attempt = 0; attempt < 32; attempt++) {
    const spot = await createSpot(
      nodeB,
      `h2-spot-${fixtureId}-${attempt}`,
      'restart-recovery'
    );
    if (spot.nodeRid !== sourceNodeRid) return spot;
  }
  throw new Error(`ST-H2 could not place a recovery Spot outside '${sourceNodeRid}'.`);
}

async function createActorOutsideRecoveryNode(
  fixtureId: string
): Promise<{ actorId: string; source: Awaited<ReturnType<typeof createActor>> }> {
  for (let attempt = 0; attempt < 32; attempt++) {
    const actorId = `h2-actor-${fixtureId}-${attempt}`;
    const source = await createActor(
      nodeA,
      actorId,
      SpotActorTransferNames.actorTypeStateful,
      121
    );
    // TransferRecoveryUserSpot is intentionally registered only by actor-b.
    // This makes ST-H2 test that process restart instead of another eligible
    // process taking over the durable authority while actor-b is offline.
    if (source.nodeRid !== 'actor-b') return { actorId, source };
  }
  throw new Error("ST-H2 could not place its source Actor outside 'actor-b'.");
}

function requireFixtureId(): string {
  return requireOption(options.fixtureId, 'fixtureId');
}

function requireOption(value: string | undefined, name: string): string {
  require(value !== undefined && value.length > 0, `ST-H2 requires e2e.${name}.`);
  return value;
}
