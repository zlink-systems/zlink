export const Names = {
  mesh: 'my-game-world',
  stream: 'players',
  actor: 'Player',
  party: 'Party',
  dungeon: 'Dungeon',
  partyPrefix: 'party-',
  dungeonPrefix: 'dungeon-'
} as const;
export const Game = { maximumPartyMembers: 4 } as const;
export const Rejection = {
  joinRejected: 'join-rejected',
  invalidParty: 'invalid-party',
  notLeader: 'not-leader',
  notReady: 'not-ready'
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
export const Scenarios = ['party', 'reconnect', 'update'] as const;
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
  lobby: 'server/lobby/main.ts',
  instance: 'server/instance/main.ts'
} as const;
export type NodeRole = keyof typeof NodeEntrypoints;
export const PacketNames = {
  joinLobbyRequest: 'JoinLobbyRequest',
  lobbyView: 'LobbyView',
  createPartyRequest: 'CreatePartyRequest',
  joinPartyRequest: 'JoinPartyRequest',
  partyJoin: 'PartyJoin',
  readyRequest: 'ReadyRequest',
  enterDungeonRequest: 'EnterDungeonRequest',
  advanceRequest: 'AdvanceRequest',
  viewRequest: 'ViewRequest',
  partyView: 'PartyView',
  dungeonView: 'DungeonView',
  prepareDungeon: 'PrepareDungeon',
  dungeonPrepared: 'DungeonPrepared',
  joinDungeon: 'JoinDungeon',
  actionRejected: 'ActionRejected',
  sessionInfoRequest: 'SessionInfoRequest',
  sessionInfo: 'SessionInfo'
} as const;
