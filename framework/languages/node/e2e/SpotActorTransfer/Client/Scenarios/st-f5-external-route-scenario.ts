// ST-F5: Message Follow route cleanup 시나리오를 검증한다.
// Two consecutive relocations preserve a bounded Message Follow chain.
import {
  SpotActorTransferNames,
  actorNode,
  armTransportDelivery,
  createActor,
  createSpotOutside,
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

export async function runStF5(scenario = 'ST-F5'): Promise<void> {
  const actorId = unique('actor-message-follow-chain');
  const actor = await createActor(
    nodeA,
    actorId,
    SpotActorTransferNames.actorTypeStateful,
    105
  );
  const sourceNode = actorNode(actor.nodeRid);
  const firstSpot = await createSpotOutside([actor.nodeRid]);
  const firstTarget = actorNode(firstSpot.nodeRid);
  const finalSpot = await createSpotOutside([actor.nodeRid, firstSpot.nodeRid]);
  const finalTarget = actorNode(finalSpot.nodeRid);
  const chainOperation = unique('delivery-chain');
  const expiredOperation = unique('delivery-expired');
  const requestOperation = unique('delivery-request');

  await armTransportDelivery(sourceNode, chainOperation, actorId, 'oneWay');
  await armTransportDelivery(sourceNode, requestOperation, actorId, 'request');
  await armTransportDelivery(sourceNode, expiredOperation, actorId, 'request');
  const chained = sendHandoffWithTransportGate(
    sourceNode,
    actorId,
    scenario,
    'chain-to-final'
  );
  const positiveRequest = probeActorWithTransportGate(
    sourceNode,
    actorId,
    scenario,
    'chain-request-to-final'
  );
  const expired = probeActorWithTransportGate(
    sourceNode,
    actorId,
    scenario,
    'after-route-removal'
  );
  await waitTransportDelivery(sourceNode, chainOperation);
  await waitTransportDelivery(sourceNode, requestOperation);
  await waitTransportDelivery(sourceNode, expiredOperation);

  require(
    (await joinActor(sourceNode, actorId, {
      scenario,
      targetSpotId: firstSpot.spotId
    })).accepted,
    'ST-F5 first join failed.'
  );
  await waitEvidence(sourceNode, [
    `${scenario}|${actorId}|message_follow_registered|6000`
  ]);
  require(
    (await joinActor(firstTarget, actorId, {
      scenario,
      targetSpotId: finalSpot.spotId
    })).accepted,
    'ST-F5 second join failed.'
  );
  await waitEvidence(firstTarget, [
    `${scenario}|${actorId}|message_follow_registered|6000`
  ]);

  await releaseTransportDelivery(sourceNode, chainOperation);
  await chained;
  await waitEvidence(finalTarget, [
    `${scenario}|${actorId}|packet_handler|chain-to-final`
  ]);
  const finalEvidence = await getEvidence(finalTarget);
  require(
    finalEvidence.filter((entry) =>
      entry.scenario === scenario
      && entry.actorId === actorId
      && entry.kind === 'packet_handler'
      && entry.value === 'chain-to-final'
    ).length === 1,
    'ST-F5 multi-hop delivery was not handled exactly once.'
  );
  for (const previousOwner of [sourceNode, firstTarget]) {
    require(
      !(await getEvidence(previousOwner)).some((entry) =>
        entry.scenario === scenario
        && entry.actorId === actorId
        && entry.kind === 'packet_handler'
        && entry.value === 'chain-to-final'
      ),
      'ST-F5 previous owner handled followed work.'
    );
  }

  await releaseTransportDelivery(sourceNode, requestOperation);
  const positive = await positiveRequest;
  require(
    positive.succeeded === true
    && positive.response?.marker === 'chain-request-to-final'
    && positive.response.nodeRid === String(finalSpot.nodeRid),
    `ST-F5 positive chained request failed with '${positive.errorKind ?? 'invalid response'}'.`
  );
  await waitEvidence(finalTarget, [
    `${scenario}|${actorId}|packet_handler|chain-request-to-final`,
    `${scenario}|${actorId}|request_reply|chain-request-to-final`
  ]);
  require(
    (await getEvidence(finalTarget)).filter((entry) =>
      entry.scenario === scenario
      && entry.actorId === actorId
      && entry.kind === 'packet_handler'
      && entry.value === 'chain-request-to-final'
    ).length === 1,
    'ST-F5 positive chained request was not handled exactly once.'
  );
  require(
    (await getEvidence(finalTarget)).filter((entry) =>
      entry.scenario === scenario
      && entry.actorId === actorId
      && entry.kind === 'request_reply'
      && entry.value === 'chain-request-to-final'
    ).length === 1,
    'ST-F5 chained request produced more than one terminal reply.'
  );
  for (const previousOwner of [sourceNode, firstTarget]) {
    require(
      !(await getEvidence(previousOwner)).some((entry) =>
        entry.scenario === scenario
        && entry.actorId === actorId
        && entry.kind === 'packet_handler'
        && entry.value === 'chain-request-to-final'
      ),
      'ST-F5 previous owner handled the positive chained request.'
    );
  }

  const firstHopContexts = messageFollowRelayEvidence(
    await getEvidence(sourceNode),
    scenario,
    actorId
  );
  const secondHopContexts = messageFollowRelayEvidence(
    await getEvidence(firstTarget),
    scenario,
    actorId
  );
  require(
    firstHopContexts.length === 2 && secondHopContexts.length === 2,
    'ST-F5 did not record one relay context per operation and hop.'
  );
  for (const request of [false, true]) {
    const first = firstHopContexts.find(context => context.request === request);
    const second = secondHopContexts.find(context => context.request === request);
    require(first !== undefined && second !== undefined, 'ST-F5 relay context is missing.');
    require(first.hopCount === 1 && second.hopCount === 2, 'ST-F5 hop count did not increase once per owner.');
    require(first.operationId === second.operationId, 'ST-F5 changed operation ID between owners.');
    require(
      first.objectGeneration === actor.objectGeneration
      && second.objectGeneration === actor.objectGeneration,
      'ST-F5 changed ObjectGeneration between owners.'
    );
    require(first.deadlineUnixMs === second.deadlineUnixMs, 'ST-F5 changed the original deadline.');
    require(first.correlationId === second.correlationId, 'ST-F5 changed request correlation.');
    require(first.replyRouteId === second.replyRouteId, 'ST-F5 changed the reply route.');
    require(
      first.payloadChecksumSha256 === second.payloadChecksumSha256,
      'ST-F5 changed the payload checksum.'
    );
    require(
      first.targetOwner.authorityOwnerGeneration
        === second.sourceOwner.authorityOwnerGeneration,
      'ST-F5 owner fence chain is not contiguous.'
    );
    require(
      BigInt(second.targetOwner.authorityOwnerGeneration)
        > BigInt(first.targetOwner.authorityOwnerGeneration),
      'ST-F5 authority owner generation did not increase.'
    );
    require(/^[0-9a-f]{32}$/.test(first.operationId), 'ST-F5 operation ID is not canonical 128-bit hex.');
    require(
      /^[0-9a-f]{64}$/.test(first.payloadChecksumSha256),
      'ST-F5 payload checksum is not canonical SHA-256.'
    );
    if (request) {
      require(
        first.deadlineUnixMs !== undefined
        && /^[0-9a-f]{32}$/.test(first.correlationId ?? '')
        && /^[0-9a-f]{32}$/.test(first.replyRouteId ?? ''),
        'ST-F5 request did not preserve deadline, correlation, and reply route.'
      );
    }
  }

  await delay(6200);
  await releaseTransportDelivery(sourceNode, expiredOperation);
  const stale = await expired;
  require(
    stale.succeeded === false && stale.errorKind === 'actorLocationStale',
    `ST-F5 expected actorLocationStale, got '${stale.errorKind ?? 'success'}'.`
  );
  for (const node of [sourceNode, firstTarget, finalTarget]) {
    require(
      !(await getEvidence(node)).some((entry) =>
        entry.scenario === scenario
        && entry.actorId === actorId
        && entry.kind === 'packet_handler'
        && entry.value === 'after-route-removal'
      ),
      'ST-F5 expired request reached an application handler.'
    );
  }
  const chainGate = await getTransportDelivery(sourceNode, chainOperation);
  const expiredGate = await getTransportDelivery(sourceNode, expiredOperation);
  const requestGate = await getTransportDelivery(sourceNode, requestOperation);
  require(
    chainGate.capturedCount === 1
    && chainGate.releasedCount === 1
    && expiredGate.capturedCount === 1
    && expiredGate.releasedCount === 1
    && requestGate.capturedCount === 1
    && requestGate.releasedCount === 1,
    'ST-F5 did not capture and release each pre-resolved delivery exactly once.'
  );
}
