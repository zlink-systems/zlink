import {
  PacketNames,
  type ActorCallRequest,
  type ActorCallResponse,
  type ActorEvidence,
  type ActorEnsureResponse,
  type ActorPushNotify,
  type ActorRefPayload,
  type BindActorReq,
  type BindActorRes,
  type SessionBindingSnapshot
} from '../../Shared/messages';
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode,
  type ZlinkStreamConnector
} from '@zlink-systems/stream-connector';
import type { ClientOptions } from './client-options';
import { postJson } from '../../../http-client';

export type { ActorEvidence } from '../../Shared/messages';

export async function bindActor(options: ClientOptions, actor: ActorRefPayload): Promise<ZlinkStreamConnector> {
  const client = zlinkStreamConnectorFactory.create({
    endpoint: options.sessionStreamEndpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    waitTimeoutMs: 10000
  });
  await client.connect();
  const reply = await client.request({ actor } satisfies BindActorReq)
    .packetName(PacketNames.bindActor).timeout(5000).submit<BindActorRes>();
  requireCondition(reply.actorId === actor.actorId, `bind ${actor.actorId} actor mismatch.`);
  requireCondition(reply.nodeRid === actor.nodeRid, `bind ${actor.actorId} node mismatch.`);
  requireCondition(
    reply.objectGeneration === actor.objectGeneration,
    `bind ${actor.actorId} object generation mismatch.`
  );
  requireCondition(reply.boundCount >= 1, `bind ${actor.actorId} did not bind any actor.`);
  return client;
}

export async function bindingSnapshot(options: ClientOptions, actorId: string): Promise<SessionBindingSnapshot> {
  return await postJson<SessionBindingSnapshot>(`${options.sessionUrl}/bindings/snapshot`, { actorId });
}

export function assertBound(snapshot: SessionBindingSnapshot, label: string): void {
  requireCondition(snapshot.sessionIds.length === 1 && snapshot.sessionIds[0]?.trim().length > 0,
    `${label} bound-session snapshot is empty.`);
}

export function assertUnbound(snapshot: SessionBindingSnapshot, label: string): void {
  requireCondition(snapshot.sessionIds.length === 0, `${label} unexpectedly created a bound session.`);
}

export function assertSameBinding(before: SessionBindingSnapshot, after: SessionBindingSnapshot, label: string): void {
  assertBound(before, `${label} before no-bind calls`);
  assertBound(after, `${label} after no-bind calls`);
  requireCondition(before.sessionIds[0] === after.sessionIds[0], `${label} changed the bound session.`);
}

export async function assertBoundPush(
  client: ZlinkStreamConnector,
  scenario: string,
  actorId: string,
  value: string
): Promise<void> {
  const pushed = client.waitFor<ActorPushNotify>(PacketNames.actorPush)
    .where((message) => message.payload.scenario === scenario && message.payload.actorId === actorId)
    .timeout(10000).submit();
  const reply = await client.request({ scenario, actorId, value })
    .packetName(PacketNames.actorPush).timeout(5000).submit<{ readonly value: string }>();
  const notify = await pushed;
  requireCondition(reply.value === `pushed:${value}`, `${scenario} reply mismatch.`);
  requireCondition(notify.payload.value === value, `${scenario} push value mismatch.`);
}

export async function assertCall(
  options: ClientOptions,
  scenario: string,
  actorId: string,
  actor: ActorRefPayload | undefined,
  value: string,
  expected: string,
  send: boolean,
  operation?: 'send' | 'request' | 'push'
): Promise<void> {
  const response = await postJson<ActorCallResponse>(
    `${options.callerUrl}/${operation ?? (send ? 'send' : 'request')}`,
    { scenario, actorId, actor, value } satisfies ActorCallRequest
  );
  requireCondition(response.result === expected, `${scenario} expected '${expected}', got '${response.result}'.`);
  requireCondition(response.errorKind === undefined, `${scenario} unexpected error '${response.errorKind}'.`);
}

export async function ensureActor(options: ClientOptions, actorId: string): Promise<ActorEnsureResponse> {
  const response = await postJson<ActorEnsureResponse>(`${options.actorUrl}/actors/${actorId}/ensure`, {});
  requireCondition(response.actor.nodeRid.trim().length > 0, `Actor '${actorId}' node rid is empty.`);
  requireCondition(
    BigInt(response.actor.objectGeneration) > 0n,
    `Actor '${actorId}' object generation is not positive.`
  );
  requireCondition(response.actor.meshName.length > 0, `Actor '${actorId}' mesh name is empty.`);
  return response;
}

export async function assertFailure(
  options: ClientOptions,
  scenario: string,
  actorId: string,
  expectedKind: string,
  send: boolean,
  actor?: ActorRefPayload
): Promise<void> {
  const response = await postJson<ActorCallResponse>(
    `${options.callerUrl}/${send ? 'send' : 'request'}`,
    { scenario, actorId, actor, value: 'missing' } satisfies ActorCallRequest
  );
  requireCondition(response.errorKind === expectedKind, `${scenario} expected '${expectedKind}', got '${response.errorKind}'.`);
}

export function requireEvidence(evidence: readonly ActorEvidence[], scenario: string, kind: string): void {
  requireCondition(evidence.some((item) => item.scenario === scenario && item.kind === kind),
    `${scenario} ${kind} evidence missing.`);
}

export function requireNoEvidence(evidence: readonly ActorEvidence[], scenario: string): void {
  requireCondition(evidence.every((item) => item.scenario !== scenario),
    `${scenario} unexpectedly reached an actor handler.`);
}

function requireCondition(condition: boolean, message: string): void {
  if (!condition) throw new Error(message);
}
