import {
  ZlinkStreamDispatchMode,
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  type ZlinkStreamConnector
} from '@zlink-systems/stream-connector';
import {
  ObservabilityOpsNames,
  type ActorCreateReq,
  type ActorCreateRes,
  type BindActorSessionReq,
  type BindActorSessionRes,
  type BoundPushNotify,
  type BoundPushReq,
  type BoundPushRes,
  type CreateSpotReq,
  type CreateSpotRes,
  type JoinTargetReq,
  type JoinTargetRes,
  type ProbeReq,
  type ProbeRes
} from '../../Shared/messages.js';
import { browserE2eConfig } from '../../../browser-client-runtime';
import { ZLinkHttpClient, type ZLinkHttpClient as HttpClient } from '@zlink-systems/http-client';

export { ObservabilityOpsNames };

export interface ClientOptions {
  nodeAUrl: string;
  nodeBUrl: string;
  sessionAStreamEndpoint: string;
  sessionBStreamEndpoint: string;
  sessionUrl: string;
  workflowAUrl: string;
  workflowBUrl: string;
  logDir: string;
  c5Phase: string;
  scenario: string;
}

export const options = parseOptions(await browserE2eConfig());
export const nodeA = ZLinkHttpClient.create(options.nodeAUrl).timeout(40000).build();
export const nodeB = ZLinkHttpClient.create(options.nodeBUrl).timeout(40000).build();
export const session = ZLinkHttpClient.create(options.sessionUrl).timeout(40000).build();
export const workflowA = ZLinkHttpClient.create(options.workflowAUrl).timeout(40000).build();
export const workflowB = ZLinkHttpClient.create(options.workflowBUrl).timeout(40000).build();

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
    nodeRid: actor.nodeRid,
    generation: actor.generation,
    transferId
  } satisfies BindActorSessionReq).packetName(ObservabilityOpsNames.packetBindActor).submit<BindActorSessionRes>();
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
  const pushed = connector.waitFor<BoundPushNotify>(ObservabilityOpsNames.packetBoundNotify)
    .where((message) => message.payload.scenario === scenario && message.payload.marker === marker)
    .timeout(15000).submit();
  const reply = await connector.request({ scenario, marker } satisfies BoundPushReq)
    .packetName(ObservabilityOpsNames.packetBoundPush)
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
  const pushed = connector.waitFor<BoundPushNotify>(ObservabilityOpsNames.packetBoundNotify)
    .where((message) => message.payload.scenario === scenario && message.payload.marker === marker)
    .timeout(15000).submit();
  const reply = await post<BoundPushRes>(node, `/actors/${actorId}/bound-push`, { scenario, marker } satisfies BoundPushReq);
  const notify = await pushed;
  require(reply.nodeRid === expectedNode && notify.payload.nodeRid === expectedNode, `${scenario} bound push node mismatch.`);
}

export async function createSpot(node: HttpClient, spotId: string, mode = 'accept'): Promise<CreateSpotRes> {
  return await post<CreateSpotRes>(node, '/spots', { spotId, mode } satisfies CreateSpotReq);
}

export async function createActor(node: HttpClient, actorId: string, actorType: string, stateVersion: number): Promise<ActorCreateRes> {
  return await post(node, '/actors', { actorId, actorType, stateVersion } satisfies ActorCreateReq);
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

export async function post<T = unknown>(node: HttpClient, path: string, body: unknown): Promise<T> {
  return await node.post(path).body(body).fetch<T>();
}

export function unique(prefix: string): string { return `${prefix}-${crypto.randomUUID().replaceAll('-', '')}`; }
export function uniqueShort(prefix: string): string { return `${prefix}-${crypto.randomUUID().slice(0, 8)}`; }
export function delay(ms: number): Promise<void> { return new Promise((resolve) => setTimeout(resolve, ms)); }
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
    sessionAStreamEndpoint: get('sessionAStreamEndpoint'),
    sessionBStreamEndpoint: get('sessionBStreamEndpoint'),
    sessionUrl: get('sessionUrl'),
    workflowAUrl: get('workflowAUrl'),
    workflowBUrl: get('workflowBUrl'),
    logDir: get('logDir'),
    c5Phase: typeof values.c5Phase === 'string' ? values.c5Phase : 'sequential',
    scenario: typeof values.scenario === 'string' ? values.scenario : 'all'
  };
}

export async function closeScenarioClients(): Promise<void> {
  await Promise.allSettled([nodeA.close(), nodeB.close(), session.close(), workflowA.close(), workflowB.close()]);
}
