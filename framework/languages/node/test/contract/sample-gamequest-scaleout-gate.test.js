const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');

function read(relativePath) {
  return fs.readFileSync(path.join(nodeRoot, relativePath), 'utf8');
}

test('GameQuest proves concurrent players use framework-selected owner nodes', () => {
  const client = read('samples/GameQuest.Ts/Client/gamequest-client-scenario.ts');
  const store = read('samples/GameQuest.Ts/Server/Shared/Store/quest-progress-store.ts');

  assert.match(client, /await Promise\.all\(\[/);
  assert.match(store, /`owner:\$\{owner\}:\$\{playerId\}`/);
  assert.match(client, /\/\^owner:mission-\[ab\]:player-alice\$\/\.test\(entry\)/);
  assert.match(client, /\/\^owner:mission-\[ab\]:player-bob\$\/\.test\(entry\)/);
  assert.doesNotMatch(client, /NodeRid|routingId|mission-a:player-alice|mission-b:player-bob/);
});

// #664: contract §7.3 requires sync to compare against the GameplayStateStore snapshot
// (the authoritative fact), and the First Hunt target monster/area must have a single
// owner (QuestDomain) instead of being duplicated as a literal in the Application layer.
test('GameQuest sync reads the GameplayStateStore snapshot and QuestDomain is the sole owner of the First Hunt target', () => {
  const processor = read('samples/GameQuest.Ts/Server/QuestMission/Application/quest-event-processor.ts');
  const domain = read('samples/GameQuest.Ts/Server/QuestMission/Domain/quest-domain.ts');

  assert.match(
    processor,
    /this\.gameplay\.readGameplaySnapshot\(request\.playerId\)/,
    'syncProgress must read the GameplayStateStore snapshot as the authoritative fact.'
  );
  assert.doesNotMatch(
    processor,
    /monsterId === 'wolf'|areaId === 'forest'/,
    'the Application layer must not keep its own copy of the First Hunt target; QuestDomain owns it.'
  );
  assert.match(
    domain,
    /reconcileFirstHunt\(\s*playerId: string,\s*snapshot: GetGameplaySnapshotRes,/,
    'QuestDomain.reconcileFirstHunt must take the snapshot and resolve the target fact itself.'
  );
});

// #664: PlayerQuestSpot has no initialize callback in Node although the .NET reference
// (PlayerQuestSpot.OnInitializeAsync) binds the owning player identity from the Spot id
// at activation instead of waiting for the first inbound request.
test('GameQuest PlayerQuestSpot binds its player id from the Spot id in an initialize callback', () => {
  const spot = read('samples/GameQuest.Ts/Server/QuestMission/Infrastructure/ZLink/Spots/PlayerQuestSpot/player-quest-spot.ts');
  assert.match(
    spot,
    /async onInitialize\(\): Promise<void> \{/,
    'PlayerQuestSpot must implement onInitialize (.NET reference: OnInitializeAsync).'
  );
  assert.match(
    spot,
    /this\.playerId = playerIdFromQuestMissionSpotId\(this\.context\.spotId\)/,
    'onInitialize must bind playerId from the Spot id, not from a request field.'
  );
});
