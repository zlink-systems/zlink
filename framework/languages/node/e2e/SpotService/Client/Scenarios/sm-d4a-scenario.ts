// SM-D4A: Rebind 뒤 stale Session을 격리한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { ensure } from '../Support/scenario-assert';
import {
  bindActor,
  countEvidence,
  createSessionClient,
  delay,
  expectStaleActorRoute,
  getEvidence,
  pingActor,
  waitEvidence
} from '../Support/session-binding-support';

export async function runSmD4A(options: ClientOptions): Promise<void> {
  const suffix = Date.now();
  const reboundActorId = `actor-sm-d4a-rebound-${suffix}`;
  const sessionAActorId = `actor-sm-d4a-session-a-${suffix}`;
  const sessionBActorId = `actor-sm-d4a-session-b-${suffix}`;
  const sessionA = createSessionClient(options.sessionAStreamEndpoint);
  const sessionB = createSessionClient(options.sessionBStreamEndpoint);

  await sessionA.connect();
  await sessionB.connect();
  try {
    const original = await bindActor(sessionA, reboundActorId, 'play-a');
    const sessionAActor = await bindActor(sessionA, sessionAActorId, 'play-a');
    const rebound = await bindActor(sessionB, reboundActorId, 'play-a');
    await bindActor(sessionB, sessionBActorId, 'play-b');
    ensure(
      original.generation === rebound.generation,
      'SM-D4A explicit rebind changed ObjectGeneration.'
    );

    await expectStaleActorRoute(sessionA, reboundActorId, 'stale-session-a');
    const current = await pingActor(sessionB, reboundActorId, 'current-session-b');
    const other = await pingActor(sessionB, sessionBActorId, 'session-b-other');
    ensure(current.actorId === reboundActorId, 'SM-D4A current binding relay mismatch.');
    ensure(other.actorId === sessionBActorId, 'SM-D4A Session B other Actor binding changed.');

    await sessionA.close();
    await waitEvidence(
      actorOwnerUrl(options, sessionAActor.nodeRid),
      `entry-disconnected|rid=${sessionAActor.nodeRid}|actor=${sessionAActorId}`
    );
    await delay(300);
    const currentOwnerEvidence = await getEvidence(actorOwnerUrl(options, rebound.nodeRid));
    ensure(
      countEvidence(
        currentOwnerEvidence,
        `entry-disconnected|rid=${rebound.nodeRid}|actor=${reboundActorId}`
      ) === 0,
      'SM-D4A Session A late disconnect reached the Session B current binding.'
    );

    const afterLateDisconnect = await pingActor(sessionB, reboundActorId, 'after-late-disconnect');
    ensure(
      afterLateDisconnect.actorId === reboundActorId,
      'SM-D4A current binding was removed by stale lifecycle cleanup.'
    );
  } finally {
    await Promise.allSettled([sessionA.close(), sessionB.close()]);
  }

  console.log('scenario SM-D4A passed');
}

function actorOwnerUrl(options: ClientOptions, nodeRid: string): string {
  if (nodeRid === 'play-a') return options.playAUrl;
  if (nodeRid === 'play-b') return options.playBUrl;
  throw new Error(`Unexpected Actor owner '${nodeRid}'.`);
}
