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
    readonly x: number,
    readonly y: number,
    readonly error: string | null = null
  ) {}
}
class MoveMsg { constructor(readonly x: number, readonly y: number) {} }
class ZoneStateNotify { constructor(readonly zoneId: string, readonly tick: number, readonly players: PlayerView[]) {} }
class ZoneChangedNotify {
  constructor(readonly playerId: string, readonly zoneId: string) {}
}
class WorldAnnounceNotify { constructor(readonly announcementId: string, readonly text: string) {} }
class MoveRejectedNotify { constructor(readonly reason: MoveRejectReason, readonly x: number, readonly y: number) {} }

class ActorLocationProbeReq { constructor(readonly actorId: string) {} }
class ActorLocationProbeRes {
  constructor(
    readonly actorId: string,
    readonly objectGeneration: string,
    readonly nodeRid: string,
    readonly error: string | null = null
  ) {}
}
class CreateFreshActorProbeReq { constructor(readonly actorId: string) {} }
class CreateFreshActorProbeRes {
  constructor(
    readonly actorId: string,
    readonly objectGeneration: string,
    readonly nodeRid: string,
    readonly error: string | null = null
  ) {}
}
class MessageFollowProbeReq {
  constructor(readonly actorId: string, readonly probeId: string, readonly payload: string) {}
}
class MessageFollowProbeMsg {
  constructor(readonly actorId: string, readonly probeId: string, readonly payload: string) {}
}
class MessageFollowProbeRes {
  constructor(readonly probeId: string, readonly payload: string, readonly error: string | null = null) {}
}

class WatchNodesReq {}
class WatchNodesRes { constructor(readonly nodes: NodeView[]) {} }
class RelocationPairReq {}
class RelocationPairRes {
  constructor(
    readonly sourceZoneId: string,
    readonly targetZoneId: string,
    readonly sourceOwnerNodeRid: string,
    readonly targetOwnerNodeRid: string,
    readonly error: string | null = null
  ) {}
}
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
class BotTickMsg {}
class PlayerActorCreateReq { constructor(readonly playerId: string) {} }
class EnterWorldReq {
  constructor(readonly x: number, readonly y: number, readonly isBot: boolean, readonly dirX = 0, readonly dirY = 0) {}
}
class EnterWorldRes {
  constructor(
    readonly zoneId: string,
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
class EnterZoneReq {
  constructor(
    readonly playerId: string,
    readonly x: number,
    readonly y: number,
    readonly isBot: boolean,
    readonly initialEntry: boolean
  ) {}
}
class EnterZoneRes {
  constructor(readonly zoneId: string, readonly error: string | null = null) {}
}

const PacketNames = {
  actorLocationProbeReq: 'ActorLocationProbeReq', actorLocationProbeRes: 'ActorLocationProbeRes',
  createFreshActorProbeReq: 'CreateFreshActorProbeReq', createFreshActorProbeRes: 'CreateFreshActorProbeRes',
  messageFollowProbeReq: 'MessageFollowProbeReq', messageFollowProbeRes: 'MessageFollowProbeRes',
  messageFollowProbeMsg: 'MessageFollowProbeMsg',
  joinWorldReq: 'JoinWorldReq', joinWorldRes: 'JoinWorldRes', moveMsg: 'MoveMsg', zoneStateNotify: 'ZoneStateNotify',
  zoneChangedNotify: 'ZoneChangedNotify', worldAnnounceNotify: 'WorldAnnounceNotify',
  moveRejectedNotify: 'MoveRejectedNotify', watchNodesReq: 'WatchNodesReq', watchNodesRes: 'WatchNodesRes',
  relocationPairReq: 'RelocationPairReq', relocationPairRes: 'RelocationPairRes',
  nodeStatusNotify: 'NodeStatusNotify', nodeAlertNotify: 'NodeAlertNotify',
  announceWorldReq: 'AnnounceWorldReq', announceWorldRes: 'AnnounceWorldRes',
  setMaintenanceReq: 'SetMaintenanceReq', setMaintenanceRes: 'SetMaintenanceRes',
  nodeDiagnosticsReq: 'NodeDiagnosticsReq', nodeDiagnosticsRes: 'NodeDiagnosticsRes',
  worldAnnounceEvent: 'WorldAnnounceEvent',
  nodeMaintenanceChangedEvent: 'NodeMaintenanceChangedEvent', deliverAnnounceMsg: 'DeliverAnnounceMsg',
  botTickMsg: 'BotTickMsg',
  enterWorldReq: 'EnterWorldReq', enterWorldRes: 'EnterWorldRes',
  applyNodeMaintenanceReq: 'ApplyNodeMaintenanceReq', applyNodeMaintenanceRes: 'ApplyNodeMaintenanceRes',
  getNodeDiagnosticsReq: 'GetNodeDiagnosticsReq', getNodeDiagnosticsRes: 'GetNodeDiagnosticsRes',
  reportSpotEventMsg: 'ReportSpotEventMsg', reportNodeStatusMsg: 'ReportNodeStatusMsg',
  zoneBorderEvent: 'ZoneBorderEvent', enterZoneReq: 'EnterZoneReq', enterZoneRes: 'EnterZoneRes'
} as const;

export {
  ActorLocationProbeReq, ActorLocationProbeRes,
  AnnounceWorldReq, AnnounceWorldRes, ApplyNodeMaintenanceReq, ApplyNodeMaintenanceRes, BotTickMsg,
  CreateFreshActorProbeReq, CreateFreshActorProbeRes,
  DeliverAnnounceMsg, PlayerActorCreateReq, EnterWorldReq, EnterWorldRes,
  EnterZoneReq, EnterZoneRes, GetNodeDiagnosticsReq, GetNodeDiagnosticsRes, JoinWorldReq, JoinWorldRes,
  MessageFollowProbeMsg, MessageFollowProbeReq, MessageFollowProbeRes,
  MoveMsg, MoveRejectedNotify, NodeAlertNotify, NodeDiagnosticsReq, NodeDiagnosticsRes,
  NodeMaintenanceChangedEvent, NodeStatusNotify, PacketNames, ReportNodeStatusMsg, ReportSpotEventMsg,
  RelocationPairReq, RelocationPairRes, SetMaintenanceReq, SetMaintenanceRes, WatchNodesReq, WatchNodesRes, WorldAnnounceEvent,
  WorldAnnounceNotify, ZoneBorderEvent, ZoneChangedNotify, ZoneStateNotify
};
export type { NodeView, PlayerView };
