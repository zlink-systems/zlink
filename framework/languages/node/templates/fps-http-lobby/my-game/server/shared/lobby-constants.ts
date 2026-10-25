import { NodeEntrypoints as GameNodeEntrypoints } from './constants';

export const LobbyNames = {
  matchPath: '/match'
} as const;

export const Http = {
  ok: 200,
  badRequest: 400,
  notFound: 404,
  unavailable: 503,
  contentType: 'application/json',
  post: 'POST',
  protocol: 'http:'
} as const;

export const LobbyScenarios = ['lobby', 'hit', 'wall', 'input', 'reconnect', 'update'] as const;

// The schema generator collects DTO names from PacketNames in shared contracts.
export const PacketNames = {
  joinRoomRequest: 'JoinRoomRequest',
  prepareRoom: 'PrepareRoom',
  roomPrepared: 'RoomPrepared',
  httpNodeReady: 'HttpNodeReady'
} as const;

export const NodeEntrypoints = {
  ...GameNodeEntrypoints,
  'http-lobby': 'server/lobby/main.ts'
} as const;

export type NodeRole = keyof typeof NodeEntrypoints;
