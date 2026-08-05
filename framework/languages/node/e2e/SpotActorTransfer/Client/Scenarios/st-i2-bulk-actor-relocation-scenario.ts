// ST-I2: Many Actor relocations 시나리오를 검증한다.
import {
  SpotActorTransferNames,
  actorNode,
  createActor,
  nodeA,
  nodeB,
  probeActor,
  relocateHost,
  require,
  sendHandoff,
  unique,
  waitActorMoved
} from '../Support/scenario-support';

export async function runStI2(): Promise<void> {
  const actors: Awaited<ReturnType<typeof createActor>>[] = [];
  for (let index = 0; index < 64 && actors.length < 16; index++) {
    const actor = await createActor(
      nodeA,
      unique('actor-i2'),
      SpotActorTransferNames.actorTypeStateful,
      index
    );
    if (actor.nodeRid === 'actor-a') actors.push(actor);
  }
  require(actors.length === 16, 'ST-I2 could not prepare 16 source-owned Actors.');

  let control: Awaited<ReturnType<typeof createActor>> | undefined;
  for (let index = 0; index < 16 && control === undefined; index++) {
    const candidate = await createActor(
      nodeB,
      unique('actor-i2-control'),
      SpotActorTransferNames.actorTypeStateful,
      900 + index
    );
    if (candidate.nodeRid !== 'actor-a') control = candidate;
  }
  require(control !== undefined, 'ST-I2 could not prepare a control Actor outside the source host.');
  let controlCount = 0;
  let movingCount = 0;
  let stop = false;
  const traffic = (async () => {
    while (!stop) {
      const moving = actors[movingCount % actors.length]!;
      await Promise.all([
        probeActor(actorNode(control.nodeRid), control.actorId, 'ST-I2', `control-${controlCount}`),
        sendHandoff(nodeA, moving.actorId, 'ST-I2', `moving-${movingCount}`)
      ]);
      controlCount++;
      movingCount++;
    }
  })();

  const startedAt = Date.now();
  const relocation = await relocateHost(nodeA);
  stop = true;
  await traffic;
  require(relocation.relocated, `ST-I2 host relocation failed: ${relocation.reason}.`);
  const moved = await Promise.all(
    actors.map(actor => waitActorMoved(nodeB, actor.actorId, 'actor-a'))
  );
  require(
    moved.every((actor, index) =>
      actor.objectGeneration === actors[index]!.objectGeneration
      && actor.nodeRid !== 'actor-a'),
    'ST-I2 changed ObjectGeneration or retained a source owner.'
  );
  require(controlCount > 0 && movingCount > 0, 'ST-I2 did not maintain control and moving traffic.');
  console.log(
    `bulk_actor_relocation units=${actors.length} elapsed_ms=${Date.now() - startedAt}`
      + ` control_requests=${controlCount} moving_one_way=${movingCount}`
  );
}
