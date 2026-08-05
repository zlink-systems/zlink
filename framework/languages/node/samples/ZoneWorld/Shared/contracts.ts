import type { MoveRejectReason, NodeAlertKind } from './spec';

type PlayerView = { playerId: string; x: number; y: number; zoneId: string; isBot: boolean };
type NodeView = {
  nodeId: string;
  registered: boolean;
  connected: boolean;
  maintenance: boolean;
  zones: string[];
  playerCount: number;
};

class JoinWorldReq { constructor(readonly playerId: string) {} }
class JoinWorldRes {
  constructor(
    readonly playerId: string,
    readonly zoneId: string,
    readonly nodeId: string,
    readonly x: number,
    readonly y: number,
    readonly error: string | null = null
  ) {}
}
class MoveMsg { constructor(readonly x: number, readonly y: number) {} }
class ZoneStateNotify { constructor(readonly zoneId: string, readonly tick: number, readonly players: PlayerView[]) {} }
class ZoneChangedNotify {
  constructor(readonly playerId: string, readonly zoneId: string, readonly nodeId: string, readonly transferred: boolean) {}
}
class WorldAnnounceNotify { constructor(readonly announcementId: string, readonly text: string) {} }
class MoveRejectedNotify { constructor(readonly reason: MoveRejectReason, readonly x: number, readonly y: number) {} }

class WatchNodesReq {}
class WatchNodesRes { constructor(readonly nodes: NodeView[]) {} }
class NodeStatusNotify {
  constructor(
    readonly nodeId: string,
    readonly registered: boolean,
    readonly connected: boolean,
    readonly maintenance: boolean,
    readonly zones: string[],
    readonly playerCount: number
  ) {}
}
class NodeAlertNotify {
  constructor(
    readonly nodeId: string,
    readonly kind: NodeAlertKind,
    readonly detail: string,
    readonly occurredAt: string
  ) {}
}
class AnnounceWorldReq { constructor(readonly text: string) {} }
class AnnounceWorldRes { constructor(readonly announcementId: string) {} }
class SetMaintenanceReq { constructor(readonly nodeId: string, readonly enabled: boolean) {} }
class SetMaintenanceRes {
  constructor(
    readonly nodeId: string,
    readonly enabled: boolean,
    readonly zones: string[],
    readonly error: string | null = null
  ) {}
}
class NodeDiagnosticsReq { constructor(readonly nodeId: string) {} }
class NodeDiagnosticsRes {
  constructor(
    readonly nodeId: string,
    readonly zones: string[],
    readonly playerCount: number,
    readonly maintenance: boolean,
    readonly error: string | null = null
  ) {}
}

class WorldAnnounceEvent { constructor(readonly announcementId: string, readonly text: string) {} }
class NodeMaintenanceChangedEvent { constructor(readonly nodeId: string, readonly enabled: boolean) {} }
class DeliverAnnounceMsg { constructor(readonly announcementId: string, readonly text: string) {} }
class BotTickReq {}
class BotTickRes {}
class PlayerActorCreateReq { constructor(readonly playerId: string) {} }
class EnterWorldReq {
  constructor(readonly x: number, readonly y: number, readonly isBot: boolean, readonly dirX = 0, readonly dirY = 0) {}
}
class EnterWorldRes {
  constructor(
    readonly zoneId: string,
    readonly nodeId: string,
    readonly x: number,
    readonly y: number,
    readonly error: string | null = null
  ) {}
}
class ApplyNodeMaintenanceReq { constructor(readonly nodeId: string, readonly enabled: boolean) {} }
class ApplyNodeMaintenanceRes {
  constructor(readonly nodeId: string, readonly enabled: boolean, readonly zones: string[]) {}
}
class GetNodeDiagnosticsReq { constructor(readonly nodeId: string) {} }
class GetNodeDiagnosticsRes {
  constructor(
    readonly nodeId: string,
    readonly zones: string[],
    readonly playerCount: number,
    readonly maintenance: boolean
  ) {}
}
class ReportSpotEventMsg {
  constructor(readonly nodeId: string, readonly kind: string, readonly detail: string, readonly occurredAt: string) {}
}
class ReportNodeStatusMsg {
  constructor(
    readonly nodeId: string,
    readonly zones: string[],
    readonly playerCount: number,
    readonly maintenance: boolean
  ) {}
}
class ZoneBorderEvent {
  constructor(readonly fromZoneId: string, readonly toZoneId: string, readonly tick: number, readonly players: PlayerView[]) {}
}
class EnterZoneMsg {
  constructor(
    readonly playerId: string,
    readonly x: number,
    readonly y: number,
    readonly isBot: boolean,
    readonly fromNodeId: string | null
  ) {}
}
class EnterZoneRes {
  constructor(readonly zoneId: string, readonly nodeId: string, readonly error: string | null = null) {}
}

const PacketNames = {
  joinWorldReq: 'JoinWorldReq', joinWorldRes: 'JoinWorldRes', moveMsg: 'MoveMsg', zoneStateNotify: 'ZoneStateNotify',
  zoneChangedNotify: 'ZoneChangedNotify', worldAnnounceNotify: 'WorldAnnounceNotify',
  moveRejectedNotify: 'MoveRejectedNotify', watchNodesReq: 'WatchNodesReq', watchNodesRes: 'WatchNodesRes',
  nodeStatusNotify: 'NodeStatusNotify', nodeAlertNotify: 'NodeAlertNotify',
  announceWorldReq: 'AnnounceWorldReq', announceWorldRes: 'AnnounceWorldRes',
  setMaintenanceReq: 'SetMaintenanceReq', setMaintenanceRes: 'SetMaintenanceRes',
  nodeDiagnosticsReq: 'NodeDiagnosticsReq', nodeDiagnosticsRes: 'NodeDiagnosticsRes',
  worldAnnounceEvent: 'WorldAnnounceEvent',
  nodeMaintenanceChangedEvent: 'NodeMaintenanceChangedEvent', deliverAnnounceMsg: 'DeliverAnnounceMsg',
  botTickReq: 'BotTickReq', botTickRes: 'BotTickRes',
  enterWorldReq: 'EnterWorldReq', enterWorldRes: 'EnterWorldRes',
  applyNodeMaintenanceReq: 'ApplyNodeMaintenanceReq', applyNodeMaintenanceRes: 'ApplyNodeMaintenanceRes',
  getNodeDiagnosticsReq: 'GetNodeDiagnosticsReq', getNodeDiagnosticsRes: 'GetNodeDiagnosticsRes',
  reportSpotEventMsg: 'ReportSpotEventMsg', reportNodeStatusMsg: 'ReportNodeStatusMsg',
  zoneBorderEvent: 'ZoneBorderEvent', enterZoneMsg: 'EnterZoneMsg', enterZoneRes: 'EnterZoneRes'
} as const;

export {
  AnnounceWorldReq, AnnounceWorldRes, ApplyNodeMaintenanceReq, ApplyNodeMaintenanceRes, BotTickReq, BotTickRes,
  DeliverAnnounceMsg, PlayerActorCreateReq, EnterWorldReq, EnterWorldRes,
  EnterZoneMsg, EnterZoneRes, GetNodeDiagnosticsReq, GetNodeDiagnosticsRes, JoinWorldReq, JoinWorldRes,
  MoveMsg, MoveRejectedNotify, NodeAlertNotify, NodeDiagnosticsReq, NodeDiagnosticsRes,
  NodeMaintenanceChangedEvent, NodeStatusNotify, PacketNames, ReportNodeStatusMsg, ReportSpotEventMsg,
  SetMaintenanceReq, SetMaintenanceRes, WatchNodesReq, WatchNodesRes, WorldAnnounceEvent,
  WorldAnnounceNotify, ZoneBorderEvent, ZoneChangedNotify, ZoneStateNotify
};
export type { NodeView, PlayerView };
