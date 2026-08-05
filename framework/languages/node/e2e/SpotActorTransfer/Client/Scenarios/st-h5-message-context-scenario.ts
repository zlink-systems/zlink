// ST-H5: MessageContext parity 시나리오를 검증한다.
import type { ProbeReq, ProbeRes } from '../../Shared/messages.js';
import {
  SpotActorTransferNames,
  actorNode,
  createActor,
  createRemoteSpot,
  getEvidence,
  joinActor,
  nodeA,
  post,
  require,
  unique,
  waitEvidence
} from '../Support/scenario-support';

interface ContextEvidence {
  readonly meshName?: string;
  readonly channelName?: string;
  readonly packetName: string;
  readonly contentType?: string;
  readonly metadata: readonly [string, string][];
  readonly correlationId?: string;
}

export async function runStH5(): Promise<void> {
  const actorId = unique('actor-h5');
  const actor = await createActor(
    nodeA,
    actorId,
    SpotActorTransferNames.actorTypeStateful,
    205
  );
  const source = actorNode(actor.nodeRid);
  await submitContextPair(source, actorId, 'entry');
  const targetSpot = await createRemoteSpot(actor.nodeRid);
  require((await joinActor(source, actorId, {
    scenario: 'ST-H5',
    targetSpotId: targetSpot.spotId
  })).accepted, 'ST-H5 relocation failed.');
  const target = actorNode(targetSpot.nodeRid);
  await waitEvidence(target, [
    `ST-H5|${actorId}|join_completion|accepted|`
  ]);
  await submitContextPair(target, actorId, 'user');
}

async function submitContextPair(
  node: ReturnType<typeof actorNode>,
  actorId: string,
  phase: string
): Promise<void> {
  const requestMarker = `${phase}-request`;
  const sendMarker = `${phase}-send`;
  const request = post<ProbeRes>(node, `/actors/${actorId}/probe`, {
    scenario: 'ST-H5',
    marker: requestMarker,
    metadata: { phase, operation: 'request' }
  } satisfies ProbeReq & { metadata: Record<string, string> });
  await post(node, `/actors/${actorId}/handoff`, {
    scenario: 'ST-H5',
    marker: sendMarker,
    metadata: { phase, operation: 'send' }
  });
  require((await request).marker === requestMarker, `ST-H5 ${phase} request reply mismatch.`);
  await waitEvidence(node, [
    `ST-H5|${actorId}|request_context|`,
    `ST-H5|${actorId}|send_context|`
  ]);
  const entries = await getEvidence(node);
  const requestContext = context(entries, actorId, 'request_context', requestMarker);
  const sendContext = context(entries, actorId, 'send_context', sendMarker);
  require(
    requestContext.meshName === SpotActorTransferNames.mesh
    && requestContext.channelName === undefined
    && requestContext.packetName === SpotActorTransferNames.packetProbe
    && requestContext.correlationId !== undefined
    && requestContext.metadata.some(([key, value]) => key === 'phase' && value === phase)
    && requestContext.metadata.some(([key, value]) => key === 'operation' && value === 'request'),
    `ST-H5 ${phase} request MessageContext mismatch.`
  );
  require(
    sendContext.meshName === SpotActorTransferNames.mesh
    && sendContext.channelName === undefined
    && sendContext.packetName === SpotActorTransferNames.packetHandoff
    && sendContext.correlationId === undefined
    && sendContext.metadata.some(([key, value]) => key === 'phase' && value === phase)
    && sendContext.metadata.some(([key, value]) => key === 'operation' && value === 'send'),
    `ST-H5 ${phase} send MessageContext mismatch.`
  );
}

function context(
  entries: Awaited<ReturnType<typeof getEvidence>>,
  actorId: string,
  kind: string,
  marker: string
): ContextEvidence {
  const markerIndex = entries.findIndex(entry =>
    entry.actorId === actorId
    && entry.kind.endsWith('packet_handler')
    && entry.value === marker
  );
  const found = entries.find((entry, index) =>
    index > markerIndex
    && entry.actorId === actorId
    && entry.kind === kind
  );
  require(found !== undefined, `ST-H5 ${kind} evidence is missing.`);
  return JSON.parse(found.value) as ContextEvidence;
}
