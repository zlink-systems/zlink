import {
  ZlinkStreamDispatchMode,
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  type ZlinkStreamConnector
} from '@zlink-systems/stream-connector';
import {
  SpotActorTransferNames,
  type ActorCreateReq,
  type ActorCreateRes,
  type ActorEvidence,
  type ActorRefRes,
  type BindActorSessionReq,
  type BindActorSessionRes,
  type BoundPushNotify,
  type BoundPushReq,
  type BoundPushRes,
  type CreateSpotReq,
  type CreateSpotRes,
  type EvidenceWaitReq,
  type GateReleaseRes,
  type JoinTargetReq,
  type JoinTargetRes,
  type ProbeReq,
  type ProbeRes
} from '../../Shared/messages.js';
import { browserE2eConfig } from '../../../browser-client-runtime';
import { ZLinkHttpClient, type ZLinkHttpClient as HttpClient } from '@zlink-systems/http-client';

export { SpotActorTransferNames };

export interface ClientOptions {
  nodeAUrl: string;
  nodeBUrl: string;
  nodeCUrl: string;
  sessionAStreamEndpoint: string;
  sessionBStreamEndpoint: string;
  scenario: string;
  fixtureId?: string;
  actorId?: string;
  targetNodeRid?: string;
  expectedObjectGeneration?: string;
  expectedOperationId?: string;
}

export const options = parseOptions(await browserE2eConfig());
export const nodeA = ZLinkHttpClient.create(options.nodeAUrl).timeout(40000).build();
export const nodeB = ZLinkHttpClient.create(options.nodeBUrl).timeout(40000).build();
export const nodeC = ZLinkHttpClient.create(options.nodeCUrl).timeout(40000).build();

export async function runRemoteTransfer(
  scenario: string,
  actorId: string,
  actorType: string,
  stateVersion: number,
  stateful: boolean,
  stageAcceptedBacklog = false
): Promise<void> {
  const sourceActor = await createActor(nodeA, actorId, actorType, stateVersion);
  const sourceNode = actorNode(sourceActor.nodeRid);
  const targetSpot = await createRemoteSpot(sourceActor.nodeRid);
  const targetNode = actorNode(targetSpot.nodeRid);
  await waitSpotRef(sourceNode, targetSpot.spotId, targetSpot.nodeRid);
  const join = joinActor(sourceNode, actorId, { scenario, targetSpotId: targetSpot.spotId });
  if (stageAcceptedBacklog) {
    await waitEvidence(sourceNode, [
      `${scenario}|${actorId}|before_commit_gate|${stateVersion}`
    ]);
    await sendHandoff(sourceNode, actorId, scenario, 'accepted-before-seal');
    await post(sourceNode, `/transfer-gates/${actorId}/release`, {});
  }
  require((await join).accepted, `${scenario} join submission failed.`);
  await waitEvidence(targetNode, [
    `${scenario}|${actorId}|join_completion|accepted|`
  ]);
  const probe = await probeActor(targetNode, actorId, scenario, 'after-transfer');
  const authority = await waitActorRef(sourceNode, actorId, targetSpot.nodeRid);
  require(
    probe.nodeRid === targetSpot.nodeRid && (!stateful || probe.stateVersion === stateVersion),
    `${scenario} target state mismatch.`
  );
  require(
    authority.objectGeneration === sourceActor.objectGeneration,
    `${scenario} changed the Actor object generation during relocation.`
  );
  const source = await waitEvidence(sourceNode, [
    `${scenario}|${actorId}|success_reply|${targetSpot.spotId}`,
    `transfer|${actorId}|transfer_out|${stateVersion}`,
    `transfer|${actorId}|leave|${stateVersion}`,
    `${scenario}|${actorId}|commit_request|after-source-leave`
  ]);
  const target = await waitEvidence(targetNode, [
    `${scenario}|${actorId}|admission|spot=${targetSpot.spotId}`,
    `${scenario}|${actorId}|deferred_completion_staged|`,
    `transfer|${actorId}|transfer_in|${stateVersion}`,
    `transfer|${actorId}|joined|${targetSpot.spotId}:${stateVersion}`,
    ...(stageAcceptedBacklog ? [
      `${scenario}|${actorId}|backlog_enqueued|0`,
      `${scenario}|${actorId}|packet_handler|accepted-before-seal`
    ] : []),
    `${scenario}|${actorId}|commit_ack|${targetSpot.spotId}`
  ]);
  require(source.length > 0 && target.length > 0, `${scenario} transfer evidence missing.`);
  assertOrder(source, actorId, [
    'transfer_out',
    'leave',
    'commit_request'
  ]);
  assertOrder(target, actorId, [
    'admission',
    'deferred_completion_staged',
    'transfer_in',
    'joined',
    ...(stageAcceptedBacklog ? ['backlog_enqueued', 'packet_handler'] : []),
    'commit_ack',
    'join_completion'
  ]);
  assertOrder(mergeEvidence(source, target), actorId, [
    'transfer_out',
    'admission',
    'deferred_completion_staged',
    'transfer_in',
    'joined',
    ...(stageAcceptedBacklog ? ['backlog_enqueued', 'packet_handler'] : []),
    'commit_ack',
    'join_completion'
  ]);
}

export async function createRemoteSpot(sourceNodeRid: string): Promise<CreateSpotRes> {
  for (let attempt = 0; attempt < 32; attempt++) {
    const spot = await createSpot(nodeB, unique('spot-remote'));
    if (spot.nodeRid !== sourceNodeRid) return spot;
  }
  throw new Error(`Could not place a Spot outside source node '${sourceNodeRid}'.`);
}

export function actorNode(nodeRid: string): HttpClient {
  if (nodeRid === 'actor-a') return nodeA;
  if (nodeRid === 'actor-b') return nodeB;
  if (nodeRid === 'actor-c') return nodeC;
  throw new Error(`Unknown actor node '${nodeRid}'.`);
}

export async function createSpotOutside(excludedNodeRids: readonly string[]): Promise<CreateSpotRes> {
  for (let attempt = 0; attempt < 64; attempt++) {
    const spot = await createSpot(nodeB, unique('spot-remote'));
    if (!excludedNodeRids.includes(spot.nodeRid)) return spot;
  }
  throw new Error(`Could not place a Spot outside nodes '${excludedNodeRids.join(',')}'.`);
}

export async function assertSourceFailure(
  label: string,
  actorType: string,
  expectedKind: string,
  targetCommitted = false
): Promise<void> {
  const actorId = unique(`actor-fail-${label}`);
  const spotId = unique(`spot-fail-${label}`);
  await createSpot(nodeB, spotId);
  await createActor(nodeA, actorId, actorType, 70);
  require(!(await joinActor(nodeA, actorId, { scenario: 'ST-C3', targetSpotId: spotId })).accepted, `ST-C3 ${label} failure returned success.`);
  const source = await getEvidence(nodeA);
  const target = await getEvidence(nodeB);
  require(has(source, actorId, expectedKind), `ST-C3 ${label} evidence missing.`);
  if (!targetCommitted) {
    require(!has(target, actorId, 'joined'), `ST-C3 target joined after ${label} failure.`);
    return;
  }
  require(has(target, actorId, 'joined'), `ST-C3 target membership missing after ${label} failure.`);
  require(has(target, actorId, 'location_committed'), `ST-C3 target location missing after ${label} failure.`);
  let dispatched = false;
  try {
    await probeActor(nodeB, actorId, 'ST-C3', `after-${label}-failure`);
    dispatched = true;
  } catch {
    // A committed-but-unreconciled target must not dispatch application packets.
  }
  require(!dispatched, `ST-C3 target dispatched while ${label} failure reconciliation is pending.`);
}

export async function connectAndBind(
  endpoint: string,
  scenario: string,
  actor: ActorCreateRes,
  transferId: string
): Promise<ZlinkStreamConnector> {
  const connector = zlinkStreamConnectorFactory.create({
    endpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    waitTimeoutMs: 15000,
    requestTimeoutMs: 10000
  });
  await connector.connect();
  const bound = await connector.request({
    scenario,
    actorId: actor.actorId,
    objectGeneration: actor.objectGeneration,
    meshName: actor.meshName,
    nodeRid: actor.nodeRid,
    transferId
  } satisfies BindActorSessionReq).packetName(SpotActorTransferNames.packetBindActor).submit<BindActorSessionRes>();
  require(bound.actorId === actor.actorId, `${scenario} session bind mismatch.`);
  await delay(500);
  return connector;
}

export async function assertBoundPush(
  connector: ZlinkStreamConnector,
  _node: HttpClient,
  actorId: string,
  scenario: string,
  marker: string,
  expectedNode: string
): Promise<void> {
  const pushed = connector.waitFor<BoundPushNotify>(SpotActorTransferNames.packetBoundNotify)
    .where((message) => message.payload.scenario === scenario && message.payload.marker === marker)
    .timeout(15000).submit();
  const reply = await connector.request({ scenario, marker } satisfies BoundPushReq)
    .packetName(SpotActorTransferNames.packetBoundPush)
    .timeout(15000)
    .submit<BoundPushRes>();
  const notify = await pushed;
  require(reply.nodeRid === expectedNode && notify.payload.nodeRid === expectedNode, `${scenario} bound push node mismatch.`);
}

export async function assertHttpBoundPush(
  connector: ZlinkStreamConnector,
  node: HttpClient,
  actorId: string,
  scenario: string,
  marker: string,
  expectedNode: string
): Promise<void> {
  const pushed = connector.waitFor<BoundPushNotify>(SpotActorTransferNames.packetBoundNotify)
    .where((message) => message.payload.scenario === scenario && message.payload.marker === marker)
    .timeout(15000).submit();
  const reply = await post<BoundPushRes>(node, `/actors/${actorId}/bound-push`, { scenario, marker } satisfies BoundPushReq);
  const notify = await pushed;
  require(reply.nodeRid === expectedNode && notify.payload.nodeRid === expectedNode, `${scenario} bound push node mismatch.`);
}

export async function createSpot(node: HttpClient, spotId: string, mode = 'accept'): Promise<CreateSpotRes> {
  return await post<CreateSpotRes>(node, '/spots', { spotId, mode } satisfies CreateSpotReq);
}

export async function createActor(
  node: HttpClient,
  actorId: string,
  actorType: string,
  stateVersion: number,
  applicationStateBytes?: number
): Promise<ActorCreateRes> {
  return await post(node, '/actors', {
    actorId,
    actorType,
    stateVersion,
    applicationStateBytes
  } satisfies ActorCreateReq);
}

export async function destroyActor(node: HttpClient, actorId: string): Promise<boolean> {
  const result = await post<{ readonly actorId: string; readonly destroyed: boolean }>(
    node,
    `/actors/${actorId}/destroy`,
    {}
  );
  require(result.actorId === actorId, `Destroy response actor mismatch for '${actorId}'.`);
  return result.destroyed;
}

export async function joinActor(node: HttpClient, actorId: string, request: JoinTargetReq): Promise<JoinTargetRes> {
  return await post(node, `/actors/${actorId}/join`, {
    ...request,
    transferId: request.transferId ?? uniqueShort('transfer')
  } satisfies JoinTargetReq);
}

export async function probeActor(node: HttpClient, actorId: string, scenario: string, marker: string): Promise<ProbeRes> {
  return await post(node, `/actors/${actorId}/probe`, { scenario, marker } satisfies ProbeReq);
}

export async function armTransportDelivery(
  node: HttpClient,
  operationId: string,
  actorId: string,
  kind: 'oneWay' | 'request'
): Promise<void> {
  await post(node, `/transport-delivery/${operationId}/arm`, { actorId, kind });
}

export async function waitTransportDelivery(
  node: HttpClient,
  operationId: string
): Promise<{ capturedCount: number; releasedCount: number }> {
  return await post(node, `/transport-delivery/${operationId}/wait`, {});
}

export async function releaseTransportDelivery(
  node: HttpClient,
  operationId: string,
  submissionCopies = 1
): Promise<{ capturedCount: number; releasedCount: number }> {
  return await post(node, `/transport-delivery/${operationId}/release`, { submissionCopies });
}

export async function getTransportDelivery(
  node: HttpClient,
  operationId: string
): Promise<{ capturedCount: number; releasedCount: number }> {
  return await node.get(`/transport-delivery/${operationId}`).fetch();
}

export async function sendHandoffWithTransportGate(
  node: HttpClient,
  actorId: string,
  scenario: string,
  marker: string
): Promise<void> {
  await post(node, `/actors/${actorId}/handoff`, {
    scenario,
    marker
  } satisfies ProbeReq);
}

export async function probeActorWithTransportGate(
  node: HttpClient,
  actorId: string,
  scenario: string,
  marker: string
): Promise<{
  readonly succeeded: boolean;
  readonly errorKind?: string;
  readonly response?: ProbeRes;
}> {
  return await post(node, `/actors/${actorId}/probe`, {
    scenario,
    marker,
    requestTimeoutMs: 15000
  } satisfies ProbeReq);
}

export async function sendHandoff(node: HttpClient, actorId: string, scenario: string, marker: string): Promise<void> {
  await post(node, `/actors/${actorId}/handoff`, { scenario, marker } satisfies ProbeReq);
}

export async function getRef(node: HttpClient, actorId: string): Promise<ActorRefRes> {
  return await node.get(`/actors/${actorId}/ref`).fetch<ActorRefRes>();
}

export async function waitActorRef(
  node: HttpClient,
  actorId: string,
  expectedNodeRid: string
): Promise<ActorRefRes> {
  const deadline = Date.now() + 10000;
  let last: ActorRefRes | undefined;
  while (Date.now() < deadline) {
    try {
      last = await getRef(node, actorId);
      if (last.nodeRid === expectedNodeRid) return last;
    } catch {
      // Authority publication and source cleanup may still be completing.
    }
    await delay(100);
  }
  throw new Error(
    `Actor '${actorId}' resolved to '${last?.nodeRid ?? '<none>'}' while waiting for '${expectedNodeRid}'.`
  );
}

export async function waitSpotRef(node: HttpClient, spotId: string, expectedNodeRid: string): Promise<void> {
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    const spot = await node.get(`/spots/${spotId}/ref`).fetch<{ found: boolean }>();
    if (spot.found) return;
    await delay(100);
  }
  throw new Error(`Spot '${spotId}' did not resolve while waiting for '${expectedNodeRid}'.`);
}

export async function getSpotRef(
  node: HttpClient,
  spotId: string
): Promise<{
  readonly found: boolean;
  readonly spotId?: string;
  readonly nodeRid?: string;
  readonly objectGeneration?: string;
}> {
  return await node.get(`/spots/${spotId}/ref`).fetch();
}

export async function relocateHost(
  node: HttpClient,
  deadlineMs = 120_000
): Promise<{
  readonly relocated: boolean;
  readonly outcome: string;
  readonly reason: string;
  readonly elapsedMs: number;
}> {
  return await post(node, '/relocate', { deadlineMs });
}

export async function waitActorMoved(
  node: HttpClient,
  actorId: string,
  previousNodeRid: string
): Promise<ActorRefRes> {
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    try {
      const current = await getRef(node, actorId);
      if (current.nodeRid !== previousNodeRid) return current;
    } catch {
      // Authority publication may be between its internal prepare and commit records.
    }
    await delay(100);
  }
  throw new Error(`Actor '${actorId}' did not move from '${previousNodeRid}'.`);
}

export async function waitSpotMoved(
  node: HttpClient,
  spotId: string,
  previousNodeRid: string
): Promise<Awaited<ReturnType<typeof getSpotRef>>> {
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    const current = await getSpotRef(node, spotId);
    if (current.found && current.nodeRid !== previousNodeRid) return current;
    await delay(100);
  }
  throw new Error(`Spot '${spotId}' did not move from '${previousNodeRid}'.`);
}

export async function getEvidence(node: HttpClient): Promise<readonly ActorEvidence[]> {
  return await node.get('/evidence').fetch<ActorEvidence[]>();
}

export interface MessageFollowRelayEvidence {
  readonly operationId: string;
  readonly objectGeneration: string;
  readonly sourceOwner: { readonly authorityOwnerGeneration: string };
  readonly targetOwner: { readonly authorityOwnerGeneration: string };
  readonly deadlineUnixMs?: number;
  readonly correlationId?: string;
  readonly replyRouteId?: string;
  readonly request: boolean;
  readonly hopCount: number;
  readonly payloadChecksumSha256: string;
}

export function messageFollowRelayEvidence(
  entries: readonly ActorEvidence[],
  scenario: string,
  actorId: string
): readonly MessageFollowRelayEvidence[] {
  return entries
    .filter(entry =>
      entry.scenario === scenario
      && entry.actorId === actorId
      && entry.kind === 'message_follow_relay_context'
    )
    .map(entry => JSON.parse(entry.value) as MessageFollowRelayEvidence);
}

export async function waitEvidence(node: HttpClient, containsAll: readonly string[], timeoutMilliseconds = 15000): Promise<readonly ActorEvidence[]> {
  const entries = await post<ActorEvidence[]>(node, '/evidence/wait', {
    containsAll,
    timeoutMilliseconds
  } satisfies EvidenceWaitReq);
  for (const expected of containsAll) {
    require(entries.some((entry) => text(entry).includes(expected)), `Evidence missing '${expected}'.`);
  }
  return entries;
}

export async function post<T = unknown>(node: HttpClient, path: string, body: unknown): Promise<T> {
  return await node.post(path).body(body).fetch<T>();
}

export function assertOrder(entries: readonly ActorEvidence[], actorId: string, kinds: readonly string[]): void {
  const filtered = entries.filter((entry) => entry.actorId === actorId);
  let cursor = -1;
  for (const kind of kinds) {
    const next = filtered.findIndex((entry, index) => index > cursor && entry.kind === kind);
    require(
      next > cursor,
      `Expected '${kind}' after evidence index ${cursor}; observed '${filtered.map((entry) => entry.kind).join(',')}'.`
    );
    cursor = next;
  }
}

export function assertValuesInOrder(
  entries: readonly ActorEvidence[],
  actorId: string,
  kind: string,
  values: readonly string[]
): void {
  const actual = entries
    .filter((entry) => entry.actorId === actorId && entry.kind === kind)
    .map((entry) => entry.value);
  require(
    values.every((value, index) => actual[index] === value),
    `Expected ${kind} values '${values.join(',')}', got '${actual.join(',')}'.`
  );
}

export function mergeEvidence(...groups: readonly (readonly ActorEvidence[])[]): readonly ActorEvidence[] {
  return groups.flat().sort((left, right) => {
    const byTime = BigInt(left.atNs) - BigInt(right.atNs);
    if (byTime < 0n) return -1;
    if (byTime > 0n) return 1;
    return left.sequence - right.sequence;
  });
}

export function has(entries: readonly ActorEvidence[], actorId: string, kind: string): boolean {
  return entries.some((entry) => entry.actorId === actorId && entry.kind === kind);
}

export function text(entry: ActorEvidence): string {
  return `${entry.scenario}|${entry.actorId}|${entry.kind}|${entry.value}|${entry.nodeRid}|transfer=${entry.transferId ?? '<none>'}`;
}

export function unique(prefix: string): string { return `${prefix}-${crypto.randomUUID().replaceAll('-', '')}`; }
export function uniqueShort(prefix: string): string { return `${prefix}-${crypto.randomUUID().slice(0, 8)}`; }
export function delay(ms: number): Promise<void> { return new Promise((resolve) => setTimeout(resolve, ms)); }
export async function isPending(promise: Promise<unknown>): Promise<boolean> {
  return await Promise.race([promise.then(() => false, () => false), delay(1).then(() => true)]);
}
export function require(condition: unknown, message: string): asserts condition {
  if (!condition) throw new Error(message);
}

export function parseOptions(value: unknown): ClientOptions {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) throw new Error('Client configuration must be an object.');
  const values = value as Record<string, unknown>;
  const get = (key: string): string => {
    const value = values[key];
    if (typeof value !== 'string' || value.length === 0) throw new Error(`Configuration value 'e2e.${key}' is required.`);
    return value;
  };
  return {
    nodeAUrl: get('nodeAUrl'),
    nodeBUrl: get('nodeBUrl'),
    nodeCUrl: get('nodeCUrl'),
    sessionAStreamEndpoint: get('sessionAStreamEndpoint'),
    sessionBStreamEndpoint: get('sessionBStreamEndpoint'),
    scenario: typeof values.scenario === 'string' ? values.scenario : 'all',
    fixtureId: typeof values.fixtureId === 'string' ? values.fixtureId : undefined,
    actorId: typeof values.actorId === 'string' ? values.actorId : undefined,
    targetNodeRid: typeof values.targetNodeRid === 'string' ? values.targetNodeRid : undefined,
    expectedObjectGeneration: typeof values.expectedObjectGeneration === 'string'
      ? values.expectedObjectGeneration
      : undefined,
    expectedOperationId: typeof values.expectedOperationId === 'string'
      ? values.expectedOperationId
      : undefined
  };
}

export async function closeScenarioClients(): Promise<void> {
  await Promise.allSettled([nodeA.close(), nodeB.close(), nodeC.close()]);
}
