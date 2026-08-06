// OBS-C2: Actor handoff 뒤 bound Session push를 유지한다 시나리오를 검증한다.
import {
  ObservabilityOpsNames,
  assertBoundPush,
  assertHttpBoundPush,
  connectAndBind,
  createActor,
  nodeA,
  nodeB,
  options,
  post,
  probeActor,
  require,
  unique
} from '../Support/scenario-support.js';
import { retireCompleted, startDrain, waitFor, waitForDrain } from '../Support/observability-support.js';
import { SessionKeepAlive } from '../../Shared/messages.js';
import type { ActorRefSnapshotRes } from '../../Shared/messages.js';

export async function runObsC2(): Promise<void> {
  let actorId = unique('obs-c2-actor');
  let actor = await createActor(nodeA, actorId, ObservabilityOpsNames.actorTypeStateful, 2);
  for (let attempt = 0; attempt < 8 && actor.nodeRid !== 'play-a'; attempt += 1) {
    actorId = unique('obs-c2-actor');
    actor = await createActor(nodeA, actorId, ObservabilityOpsNames.actorTypeStateful, 2);
  }
  require(actor.nodeRid === 'play-a', `OBS-C2 source actor was placed on '${actor.nodeRid}'.`);
  const visibleActor = await waitFor<ActorRefSnapshotRes | undefined>(async () => {
    try {
      return await nodeA.get(`/actors/${actorId}/ref`).fetch<ActorRefSnapshotRes>();
    } catch {
      return undefined;
    }
  }, (value) => value?.nodeRid === 'play-a' && value.objectGeneration === actor.objectGeneration,
  'OBS-C2 source Actor ref was not visible before Session bind', 10000);
  require(visibleActor !== undefined, 'OBS-C2 source Actor ref disappeared before Session bind.');
  await waitFor(async () => {
    try {
      return await probeActor(nodeA, actorId, 'OBS-C2', 'bind-ready');
    } catch {
      return undefined;
    }
  }, (value) => value !== undefined, 'OBS-C2 source Actor route was not ready before Session bind', 15000);
  const connector = await waitFor(async () => {
    try {
      return await connectAndBind(options.sessionAStreamEndpoint, 'OBS-C2', {
        ...actor,
        nodeRid: visibleActor.nodeRid,
        objectGeneration: visibleActor.objectGeneration,
        meshName: visibleActor.meshName
      }, unique('c2-bind'));
    } catch {
      return undefined;
    }
  }, (value) => value !== undefined, 'OBS-C2 Session bind did not reach the source native route', 15000);
  require(connector !== undefined, 'OBS-C2 Session connector was not created.');
  // The server deliberately closes an application-idle STREAM after 30 seconds.
  // Keep this bound Session active while the relocation barrier is running; the
  // scenario must verify route handoff, not an unrelated idle disconnect.
  const keepAlive = setInterval(() => {
    void connector.send(new SessionKeepAlive('OBS-C2'))
      .packetName(ObservabilityOpsNames.packetSessionKeepAlive)
      .submit()
      .catch(() => undefined);
  }, 5000);
  try {
    await assertBoundPush(connector, nodeA, actorId, 'OBS-C2', 'before-drain', 'play-a');
    await startDrain(nodeA, 60000);
    const complete = await waitForDrain(nodeA,
      retireCompleted,
      'OBS-C2 drain did not complete', 65000);
    await waitFor(async () => {
      try {
        return await nodeB.get(`/actors/${actorId}/ref`).fetch<ActorRefSnapshotRes>();
      } catch {
        return undefined;
      }
    }, (actorRef) => actorRef?.nodeRid === 'play-b', 'OBS-C2 actor was not handed off to play-b', 15000);
    require(retireCompleted(complete), 'OBS-C2 retire did not complete.');
    await assertHttpBoundPush(connector, nodeB, actorId, 'OBS-C2', 'after-drain', 'play-b');
    await post(nodeB, '/evidence/wait', {
      containsAll: [`OBS-C2|${actorId}|bound_push|after-drain`],
      timeoutMilliseconds: 10000
    });
  } finally {
    clearInterval(keepAlive);
    await connector.close().catch(() => undefined);
  }
}
