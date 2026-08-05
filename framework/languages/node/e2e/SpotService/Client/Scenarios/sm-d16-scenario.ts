// SM-D16: one StreamNode dispatches bound Actor packets across two Object Meshes.
import { SpotServiceNames } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { ensure } from '../Support/scenario-assert';
import {
  bindActor,
  createSessionClient,
  pingActor
} from '../Support/session-binding-support';

export async function runSmD16(options: ClientOptions): Promise<void> {
  const suffix = Date.now();
  const primaryActorId = `actor-sm-d16-primary-${suffix}`;
  const alternateActorId = `actor-sm-d16-alternate-${suffix}`;
  const client = createSessionClient(options.sessionAStreamEndpoint);
  await client.connect();
  try {
    await bindActor(client, primaryActorId, 'play-a', SpotServiceNames.spotChannel);
    await bindActor(
      client,
      alternateActorId,
      'multi-node-a',
      SpotServiceNames.spotOnlyMesh
    );

    const primary = await pingActor(client, primaryActorId, 'primary-object-mesh');
    const alternate = await pingActor(client, alternateActorId, 'alternate-object-mesh');
    ensure(primary.actorId === primaryActorId, 'SM-D16 primary Object Mesh dispatch mismatch.');
    ensure(primary.nodeRid === 'play-a', 'SM-D16 primary Actor owner mismatch.');
    ensure(
      alternate.actorId === alternateActorId,
      'SM-D16 alternate Object Mesh dispatch mismatch.'
    );
    ensure(alternate.nodeRid === 'multi-node-a', 'SM-D16 alternate Actor owner mismatch.');
  } finally {
    await client.close();
  }

  console.log('scenario SM-D16 passed');
}
