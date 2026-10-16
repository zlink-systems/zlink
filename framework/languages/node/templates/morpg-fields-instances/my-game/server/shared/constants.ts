export const Names = {
  mesh: 'my-game-world',
  stream: 'players',
  actor: 'Player',
  field: 'Field',
  dungeon: 'Dungeon',
  border: 'border',
  borderTimer: 'border-tick',
  fieldPrefix: 'field-',
  dungeonPrefix: 'dungeon-'
} as const;
export const Game = {
  fieldWidth: 100,
  fieldCount: 4,
  borderRadius: 10,
  tickMs: 100,
  maximumPartyMembers: 4
} as const;
export const Rejection = { invalidParty: 'INVALID_PARTY', notLeader: 'NOT_LEADER' } as const;
export const MessageFollowMs = 20_000;
export const Limits = {
  safeShutdownMs: MessageFollowMs + 10_000,
  observationCapacity: 64,
  messageMs: 10_000,
  startupMs: 30_000,
  relocationMs: 25_000,
  shutdownMs: 10_000
} as const;
export const Scenarios = ['field', 'dungeon', 'reconnect', 'update'] as const;
export const Control = {
  ready: 'READY ',
  drain: 'drain',
  shutdown: 'shutdown',
  requested: 'RELOCATION_REQUESTED',
  relocated: 'RELOCATED',
  safe: 'SAFE_TO_SHUTDOWN',
  stopped: 'STOPPED '
} as const;
export const NodeEntrypoints = {
  session: 'server/session/main.ts',
  field: 'server/field/main.ts',
  instance: 'server/instance/main.ts'
} as const;
export type NodeRole = keyof typeof NodeEntrypoints;
export function fieldId(x: number): string {
  if (!Number.isSafeInteger(x) || x < 0 || x >= Game.fieldWidth * Game.fieldCount)
    throw new Error('Position outside world');
  return Names.fieldPrefix + (Math.floor(x / Game.fieldWidth) + 1);
}
export function adjacentFields(id: string): string[] {
  const n = Number(id.slice(Names.fieldPrefix.length));
  return [n - 1, n + 1]
    .filter((n) => n >= 1 && n <= Game.fieldCount)
    .map((n) => Names.fieldPrefix + n);
}
export const borderTopic = (source: string, target: string): string => `${source}:${target}`;
export const dungeonId = (party: string): string => Names.dungeonPrefix + party;
export const PacketNames = {
  enterField: 'EnterField',
  borderPresence: 'BorderPresence',
  dungeonPrepared: 'DungeonPrepared',
  joinWorldRequest: 'JoinWorldRequest',
  moveRequest: 'MoveRequest',
  worldState: 'WorldState',
  borderVisible: 'BorderVisible',
  createPartyRequest: 'CreatePartyRequest',
  joinPartyRequest: 'JoinPartyRequest',
  enterDungeonRequest: 'EnterDungeonRequest',
  advanceRequest: 'AdvanceRequest',
  viewRequest: 'ViewRequest',
  fieldView: 'FieldView',
  dungeonView: 'DungeonView',
  prepareDungeon: 'PrepareDungeon',
  joinDungeon: 'JoinDungeon',
  actionRejected: 'ActionRejected',
  sessionInfoRequest: 'SessionInfoRequest',
  sessionInfo: 'SessionInfo'
} as const;
