export const Names = {
  stream: 'players',
  actor: 'Player',
  zone: 'Zone',
  zonePrefix: 'field@',
  playerKey: 'player:',
  worldsPath: '/worlds'
} as const;
export function meshName(worldId: string): string {
  return `my-game-world-${worldId.toLowerCase()}`;
}
export function zoneId(channel: bigint): string {
  if (channel < 1n) throw new Error('Invalid channel');
  return Names.zonePrefix + channel.toString();
}
export function channelOf(spotId: string): bigint {
  if (!spotId.startsWith(Names.zonePrefix)) throw new Error('Invalid Zone ID');
  const channel = BigInt(spotId.slice(Names.zonePrefix.length));
  if (zoneId(channel) !== spotId) throw new Error('Invalid Zone ID');
  return channel;
}
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
export const Scenarios = ['worlds', 'channel', 'reconnect', 'update'] as const;
export const PacketNames = {
  enterWorldRequest: 'EnterWorldRequest',
  changeChannelRequest: 'ChangeChannelRequest',
  earnRequest: 'EarnRequest',
  viewRequest: 'ViewRequest',
  channelViewRequest: 'ChannelViewRequest',
  channelView: 'ChannelView',
  actionRejected: 'ActionRejected',
  playerView: 'PlayerView',
  sessionInfoRequest: 'SessionInfoRequest',
  sessionInfo: 'SessionInfo'
} as const;
export const Control = {
  playerCreated: 'PLAYER_CREATED ',
  ready: 'READY ',
  drain: 'drain',
  createUpdateZone: 'create-update-zone',
  updateZoneCreated: 'UPDATE_ZONE_CREATED',
  shutdown: 'shutdown',
  requested: 'RELOCATION_REQUESTED',
  relocated: 'RELOCATED',
  safe: 'SAFE_TO_SHUTDOWN',
  stopped: 'STOPPED '
} as const;
export const NodeEntrypoints = {
  session: 'server/session/main.ts',
  channel: 'server/channel/main.ts',
  accounts: 'server/accounts/main.ts'
} as const;
export type NodeRole = keyof typeof NodeEntrypoints;

export const Game = { firstChannel: 1n, updateChannel: 3n } as const;
