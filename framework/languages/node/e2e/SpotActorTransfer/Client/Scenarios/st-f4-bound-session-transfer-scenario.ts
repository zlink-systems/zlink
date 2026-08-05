// ST-F4: Message Follow before and after expiry 시나리오를 검증한다.
// Two deliveries are delayed after global Actor resolution: G1 is released
// inside the Message Follow duration, while G2 is released after expiry.
import {
  SpotActorTransferNames,
  actorNode,
  armTransportDelivery,
  createActor,
  createRemoteSpot,
  delay,
  getEvidence,
  getTransportDelivery,
  joinActor,
  messageFollowRelayEvidence,
  nodeA,
  probeActorWithTransportGate,
  releaseTransportDelivery,
  require,
  sendHandoffWithTransportGate,
  unique,
  waitEvidence,
  waitTransportDelivery
} from '../Support/scenario-support';

export async function runStF4(scenario = 'ST-F4'): Promise<void> {
  const actorId = unique('actor-message-follow');
  const actor = await createActor(
    nodeA,
    actorId,
    SpotActorTransferNames.actorTypeStateful,
    104
  );
  const sourceNode = actorNode(actor.nodeRid);
  const targetSpot = await createRemoteSpot(actor.nodeRid);
  const targetNode = actorNode(targetSpot.nodeRid);
  const g1 = unique('delivery-g1');
  const g2 = unique('delivery-g2');
  const g3 = unique('delivery-g3');

  await armTransportDelivery(sourceNode, g1, actorId, 'oneWay');
  await armTransportDelivery(sourceNode, g3, actorId, 'request');
  await armTransportDelivery(sourceNode, g2, actorId, 'request');
  const g1Delivery = sendHandoffWithTransportGate(
    sourceNode,
    actorId,
    scenario,
    'G1'
  );
  const g3Delivery = probeActorWithTransportGate(
    sourceNode,
    actorId,
    scenario,
    'G3-positive-request'
  );
  const g2Delivery = probeActorWithTransportGate(
    sourceNode,
    actorId,
    scenario,
    'G2'
  );
  await waitTransportDelivery(sourceNode, g1);
  await waitTransportDelivery(sourceNode, g3);
  await waitTransportDelivery(sourceNode, g2);

  require(
    (await joinActor(sourceNode, actorId, {
      scenario,
      targetSpotId: targetSpot.spotId
    })).accepted,
    'ST-F4 join failed.'
  );
  await waitEvidence(sourceNode, [
    `${scenario}|${actorId}|message_follow_registered|6000`
  ]);

  await releaseTransportDelivery(sourceNode, g1, 2);
  await g1Delivery;
  await waitEvidence(targetNode, [
    `${scenario}|${actorId}|packet_handler|G1`
  ]);
  const delivered = await getEvidence(targetNode);
  require(
    delivered.filter((entry) =>
      entry.scenario === scenario
      && entry.actorId === actorId
      && entry.kind === 'packet_handler'
      && entry.value === 'G1'
    ).length === 1,
    'ST-F4 G1 was not handled exactly once.'
  );

  await releaseTransportDelivery(sourceNode, g3, 2);
  const positive = await g3Delivery;
  require(
    positive.succeeded === true
    && positive.response?.marker === 'G3-positive-request'
    && positive.response.nodeRid === String(targetSpot.nodeRid),
    `ST-F4 positive Message Follow request failed with '${positive.errorKind ?? 'invalid response'}'.`
  );
  await waitEvidence(targetNode, [
    `${scenario}|${actorId}|packet_handler|G3-positive-request`,
    `${scenario}|${actorId}|request_reply|G3-positive-request`
  ]);
  require(
    (await getEvidence(targetNode)).filter((entry) =>
      entry.scenario === scenario
      && entry.actorId === actorId
      && entry.kind === 'packet_handler'
      && entry.value === 'G3-positive-request'
    ).length === 1,
    'ST-F4 positive Message Follow request was not handled exactly once.'
  );
  require(
    (await getEvidence(targetNode)).filter((entry) =>
      entry.scenario === scenario
      && entry.actorId === actorId
      && entry.kind === 'request_reply'
      && entry.value === 'G3-positive-request'
    ).length === 1,
    'ST-F4 duplicate request produced more than one terminal reply.'
  );

  const relayContexts = messageFollowRelayEvidence(
    await getEvidence(sourceNode),
    scenario,
    actorId
  );
  require(relayContexts.length === 2, 'ST-F4 did not record exactly one relay for each duplicated operation.');
  const oneWayContext = relayContexts.find(context => !context.request);
  const requestContext = relayContexts.find(context => context.request);
  for (const context of [oneWayContext, requestContext]) {
    require(context !== undefined, 'ST-F4 relay context is missing.');
    require(/^[0-9a-f]{32}$/.test(context.operationId), 'ST-F4 operation ID is not canonical 128-bit hex.');
    require(context.objectGeneration === actor.objectGeneration, 'ST-F4 changed ObjectGeneration while relaying.');
    require(context.hopCount === 1, 'ST-F4 one-hop relay recorded an invalid hop count.');
    require(
      BigInt(context.targetOwner.authorityOwnerGeneration)
        > BigInt(context.sourceOwner.authorityOwnerGeneration),
      'ST-F4 authority owner generation did not increase.'
    );
    require(
      /^[0-9a-f]{64}$/.test(context.payloadChecksumSha256),
      'ST-F4 payload checksum is not canonical SHA-256.'
    );
  }
  require(
    requestContext?.deadlineUnixMs !== undefined
    && /^[0-9a-f]{32}$/.test(requestContext.correlationId ?? '')
    && /^[0-9a-f]{32}$/.test(requestContext.replyRouteId ?? ''),
    'ST-F4 request did not preserve deadline, correlation, and reply route.'
  );

  await delay(6200);
  await releaseTransportDelivery(sourceNode, g2);
  const stale = await g2Delivery;
  require(
    stale.succeeded === false && stale.errorKind === 'actorLocationStale',
    `ST-F4 expected actorLocationStale, got '${stale.errorKind ?? 'success'}'.`
  );
  const after = [
    ...await getEvidence(sourceNode),
    ...await getEvidence(targetNode)
  ];
  require(
    !after.some((entry) =>
      entry.scenario === scenario
      && entry.actorId === actorId
      && entry.kind === 'packet_handler'
      && entry.value === 'G2'
    ),
    'ST-F4 expired G2 reached an application handler.'
  );
  const g1Snapshot = await getTransportDelivery(sourceNode, g1);
  const g2Snapshot = await getTransportDelivery(sourceNode, g2);
  const g3Snapshot = await getTransportDelivery(sourceNode, g3);
  require(
    g1Snapshot.capturedCount === 1
    && g1Snapshot.releasedCount === 1
    && g2Snapshot.capturedCount === 1
    && g2Snapshot.releasedCount === 1
    && g3Snapshot.capturedCount === 1
    && g3Snapshot.releasedCount === 1,
    'ST-F4 did not capture and release each pre-resolved delivery exactly once.'
  );
}
