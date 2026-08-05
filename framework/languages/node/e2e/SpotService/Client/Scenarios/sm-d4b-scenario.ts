// SM-D4B: Relocation 뒤 stored binding route의 Message Follow를 사용한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { ensure } from '../Support/scenario-assert';
import {
  bindActor,
  createSessionClient,
  expectStaleActorRoute,
  pingActor
} from '../Support/session-binding-support';

export async function runSmD4B(options: ClientOptions): Promise<void> {
  const actorId = `actor-sm-d4b-stored-route-${Date.now()}`;
  const staleSession = createSessionClient(options.sessionAStreamEndpoint);
  const currentSession = createSessionClient(options.sessionBStreamEndpoint);
  await staleSession.connect();
  await currentSession.connect();
  try {
    const original = await bindActor(staleSession, actorId, 'play-a');
    const beforeRebind = await pingActor(staleSession, actorId, 'stored-route-before-rebind');
    ensure(beforeRebind.actorId === actorId, 'SM-D4B valid stored route relay mismatch.');

    const rebound = await bindActor(currentSession, actorId, 'play-a');
    ensure(
      original.generation === rebound.generation,
      'SM-D4B same-incarnation rebind changed ObjectGeneration.'
    );
    await expectStaleActorRoute(staleSession, actorId, 'stale-route-after-rebind');
    const current = await pingActor(currentSession, actorId, 'stored-route-after-rebind');
    ensure(current.actorId === actorId, 'SM-D4B current stored route relay mismatch.');
  } finally {
    await Promise.allSettled([staleSession.close(), currentSession.close()]);
  }

  console.log('scenario SM-D4B passed');
}
