export const Names = {
  mesh: 'my-game-world',
  stream: 'players',
  actor: 'Player',
  town: 'Town',
  dungeon: 'Dungeon'
} as const;
export const Game = { maximumPartyMembers: 4, channelCount: 2 } as const;
export const Rejection = {
  joinRejected: 'JOIN_REJECTED',
  invalidParty: 'INVALID_PARTY',
  notLeader: 'NOT_LEADER'
} as const;
export const Limits = {
  observationCapacity: 64,
  messageMs: 10_000,
  startupMs: 30_000,
  relocationMs: 25_000,
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
  actionRejected: 'ActionRejected'
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
export function townId(channel: number): string {
  if (!Number.isInteger(channel) || channel < 1 || channel > Game.channelCount)
    throw new Error('Invalid channel');
  return `town-${channel}`;
}
export function dungeonId(partyId: string): string {
  return `dungeon-${partyId}`;
}
