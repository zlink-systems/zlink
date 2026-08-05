// ST-I3: Many Spot relocations 시나리오를 검증한다.
import {
  createSpot,
  getSpotRef,
  nodeA,
  nodeB,
  relocateHost,
  require,
  unique,
  waitSpotMoved
} from '../Support/scenario-support';

export async function runStI3(): Promise<void> {
  const spots: Array<Awaited<ReturnType<typeof createSpot>> & {
    readonly objectGeneration: string;
  }> = [];
  for (let index = 0; index < 64 && spots.length < 16; index++) {
    const spot = await createSpot(nodeA, unique('spot-i3'));
    if (spot.nodeRid === 'actor-a') {
      const before = await getSpotRef(nodeA, spot.spotId);
      require(before.objectGeneration !== undefined, 'ST-I3 source Spot generation is missing.');
      spots.push({ ...spot, objectGeneration: before.objectGeneration });
    }
  }
  require(spots.length === 16, 'ST-I3 could not prepare 16 source-owned Spots.');

  const startedAt = Date.now();
  const relocation = await relocateHost(nodeA);
  require(relocation.relocated, `ST-I3 host relocation failed: ${relocation.reason}.`);
  const moved = await Promise.all(
    spots.map(spot => waitSpotMoved(nodeB, spot.spotId, 'actor-a'))
  );
  require(
    moved.every((spot, index) =>
      spot.objectGeneration === spots[index]!.objectGeneration
      && spot.nodeRid !== 'actor-a'),
    'ST-I3 changed ObjectGeneration or retained a source owner.'
  );
  console.log(
    `bulk_spot_relocation units=${spots.length} elapsed_ms=${Date.now() - startedAt}`
  );
}
