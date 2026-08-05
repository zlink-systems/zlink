// The wire contract from scenario §7, mirrored from the language-neutral definition.
// The field names are the contract: every language server encodes exactly these.
// Only the messages the browser exchanges appear here — server-internal messages
// (§7.3) never reach the client.

export interface PlayerView {
  playerId: string;
  x: number;
  y: number;
  zoneId: string;
  isBot: boolean;
}

export interface NodeView {
  nodeId: string;
  registered: boolean;
  connected: boolean;
  maintenance: boolean;
  zones: string[];
  playerCount: number;
}

// --- §7.1 game — browser <-> Gateway ---------------------------------------

export interface JoinWorldReq {
  playerId: string;
}

export interface JoinWorldRes {
  playerId: string;
  zoneId: string;
  nodeId: string;
  x: number;
  y: number;
  error?: string | null;
}

export interface MoveMsg {
  x: number;
  y: number;
}

export interface ZoneStateNotify {
  zoneId: string;
  tick: number;
  players: PlayerView[];
}

export interface ZoneChangedNotify {
  playerId: string;
  zoneId: string;
  nodeId: string;
  transferred: boolean;
}

export interface WorldAnnounceNotify {
  announcementId: string;
  text: string;
}

export interface MoveRejectedNotify {
  reason: MoveRejectReason;
  x: number;
  y: number;
}

export type MoveRejectReason =
  | 'OutOfRange'
  | 'TooFar'
  | 'DiagonalCrossing'
  | 'ZoneMaintenance';

// --- §7.2 ops console — browser <-> Ops -------------------------------------

export type WatchNodesReq = Record<string, never>;

export interface WatchNodesRes {
  nodes: NodeView[];
}

export interface NodeStatusNotify {
  nodeId: string;
  registered: boolean;
  connected: boolean;
  maintenance: boolean;
  zones: string[];
  playerCount: number;
}

export interface NodeAlertNotify {
  nodeId: string;
  kind: NodeAlertKind;
  detail: string;
  occurredAt: string;
}

export type NodeAlertKind = 'TimerHandlerFailed' | 'PeersChanged';

export interface AnnounceWorldReq {
  text: string;
}

export interface AnnounceWorldRes {
  announcementId: string;
}

export interface SetMaintenanceReq {
  nodeId: string;
  enabled: boolean;
}

export interface SetMaintenanceRes {
  nodeId: string;
  enabled: boolean;
  zones: string[];
  error?: string | null;
}

export interface NodeDiagnosticsReq {
  nodeId: string;
}

export interface NodeDiagnosticsRes {
  nodeId: string;
  zones: string[];
  playerCount: number;
  maintenance: boolean;
  error?: string | null;
}

/** A request whose target node is not connected returns this in `error`. */
export const NODE_UNAVAILABLE = 'NodeUnavailable';

/** Packet names are the contract type names; the servers register them the same way. */
export const Packets = {
  JoinWorldReq: 'JoinWorldReq',
  JoinWorldRes: 'JoinWorldRes',
  MoveMsg: 'MoveMsg',
  ZoneStateNotify: 'ZoneStateNotify',
  ZoneChangedNotify: 'ZoneChangedNotify',
  WorldAnnounceNotify: 'WorldAnnounceNotify',
  MoveRejectedNotify: 'MoveRejectedNotify',
  WatchNodesReq: 'WatchNodesReq',
  WatchNodesRes: 'WatchNodesRes',
  NodeStatusNotify: 'NodeStatusNotify',
  NodeAlertNotify: 'NodeAlertNotify',
  AnnounceWorldReq: 'AnnounceWorldReq',
  AnnounceWorldRes: 'AnnounceWorldRes',
  SetMaintenanceReq: 'SetMaintenanceReq',
  SetMaintenanceRes: 'SetMaintenanceRes',
  NodeDiagnosticsReq: 'NodeDiagnosticsReq',
  NodeDiagnosticsRes: 'NodeDiagnosticsRes',
} as const;
