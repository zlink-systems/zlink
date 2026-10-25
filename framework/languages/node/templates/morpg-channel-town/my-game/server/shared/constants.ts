export const Names = {
  mesh: 'my-game-world',
  stream: 'players',
  actor: 'Player',
  town: 'Town',
  dungeon: 'Dungeon'
} as const;
export const Game = {
  maximumPartyMembers: 4,
  channelCount: 2,
  maximumInteger: 9_007_199_254_740_991n,
  finalStage: 3n
} as const;
export const Rejection = {
  joinRejected: 'join-rejected',
  invalidParty: 'invalid-party',
  notLeader: 'not-leader',
  runMismatch: 'run-mismatch',
  runInProgress: 'run-in-progress',
  runNotActive: 'run-not-active',
  integerOutOfRange: 'integer-out-of-range',
  invalidChannel: 'invalid-channel',
  packetNotAllowed: 'packet-not-allowed'
} as const;
const messageFollowMs = 30_000;
const relocationMs = 25_000;
export const Limits = {
  messageFollowMs,
  safeMs: messageFollowMs + relocationMs,
  observationCapacity: 64,
  messageMs: 10_000,
  startupMs: 30_000,
  relocationMs,
  shutdownMs: 10_000
} as const;
export const Scenarios = ['channel', 'dungeon', 'reconnect', 'update'] as const;
export const PacketNames = {
  sessionInfoRequest: 'SessionInfoRequest',
  sessionInfo: 'SessionInfo',
  joinTownRequest: 'JoinTownRequest',
  changeChannelRequest: 'ChangeChannelRequest',
  createPartyRequest: 'CreatePartyRequest',
  joinPartyRequest: 'JoinPartyRequest',
  enterDungeonRequest: 'EnterDungeonRequest',
  advanceRequest: 'AdvanceRequest',
  viewRequest: 'ViewRequest',
  townView: 'TownView',
  dungeonView: 'DungeonView',
  prepareDungeon: 'PrepareDungeon',
  dungeonPrepared: 'DungeonPrepared',
  joinDungeon: 'JoinDungeon',
  actionRejected: 'ActionRejected',
  runStarted: 'RunStarted',
  runCompleted: 'RunCompleted',
  partyViewRequest: 'PartyViewRequest'
} as const;
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
  town: 'server/town/main.ts',
  instance: 'server/instance/main.ts'
} as const;
export type NodeRole = keyof typeof NodeEntrypoints;
export function townId(channel: bigint): string {
  if (channel < 1n || channel > BigInt(Game.channelCount))
    throw new Error('Invalid channel');
  return `town-${channel}`;
}
export function dungeonId(runId: string): string {
  return `dungeon-${runId}`;
}
