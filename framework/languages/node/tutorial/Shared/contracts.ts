// Message contracts shared by both processes. The Framework serializes them,
// and nothing here is annotated.
//
// A message the caller *sends* is a class, not an interface: the Framework
// reads the wire packet name off `payload.constructor.name`, and an object
// literal's constructor is `Object`, which it refuses. A message that is only
// *received* is decoded by shape, so it stays an interface.

// --8<-- [start:channel-contracts]
export class GetPlayerProfile {
  constructor(readonly playerId: string) {}
}

export interface PlayerProfile {
  readonly playerId: string;
  readonly nickname: string;
  readonly level: number;
}

// One-way: the caller does not wait, so this message has no reply type.
export class RecordLogin {
  constructor(readonly playerId: string) {}
}
// --8<-- [end:channel-contracts]

// --8<-- [start:clientserver-contracts]
export class IssueSessionTicket {
  constructor(readonly playerId: string) {}
}

export interface SessionTicket {
  readonly value: string;
}
// --8<-- [end:clientserver-contracts]

// --8<-- [start:fanout-contracts]
// Published without naming a recipient. Every subscribed node receives it.
export class MaintenanceNotice {
  constructor(readonly message: string) {}
}
// --8<-- [end:fanout-contracts]

// --8<-- [start:spot-contracts]
// Reaches the room's create callback rather than a handler, so it carries what
// the room needs in order to exist.
export class OpenRoom {
  constructor(readonly title: string) {}
}

// One-way: the caller does not wait for the room to record the line.
export class PostChat {
  constructor(
    readonly playerId: string,
    readonly text: string
  ) {}
}

export class GetRoomState {}

export interface RoomState {
  readonly title: string;
  readonly chat: readonly string[];
}
// --8<-- [end:spot-contracts]

// --8<-- [start:instance-spot-contracts]
// A match queue has no create call, so nothing here corresponds to OpenRoom.
export class JoinMatchQueue {
  constructor(readonly playerId: string) {}
}

export interface MatchQueueStatus {
  readonly waiting: number;
}
// --8<-- [end:instance-spot-contracts]

// --8<-- [start:actor-contracts]
// Reaches the player's create callback rather than a handler.
export class CreatePlayer {
  constructor(readonly nickname: string) {}
}

// One-way: the caller does not wait for the rename to be recorded.
export class ChangeNickname {
  constructor(readonly nickname: string) {}
}

export class GetPlayer {}

export interface PlayerInfo {
  readonly playerId: string;
  readonly nickname: string;
}
// --8<-- [end:actor-contracts]

// --8<-- [start:stream-contracts]
// Exchanged over the external TCP connection, not between mesh nodes. The
// stream codec wants 64-bit integers as decimal strings, so the timestamp is
// carried as text rather than as a number.
export class Ping {
  constructor(readonly sentAtUnixMs: string) {}
}

// Sent by the server, so it is a class as well: the reply carries a packet
// name taken from the constructor.
export class Pong {
  constructor(readonly sentAtUnixMs: string) {}
}
// --8<-- [end:stream-contracts]

// --8<-- [start:session-actor-contracts]
export class Authenticate {
  constructor(readonly playerId: string) {}
}

export class Authenticated {
  constructor(readonly playerId: string) {}
}

// Pushed by the player to its own connection, with no request to answer.
export class NicknameChanged {
  constructor(readonly nickname: string) {}
}
// --8<-- [end:session-actor-contracts]

// --8<-- [start:node-direct-contracts]
// Answered by the node itself rather than by a channel, so the reply describes
// that one process.
export class GetNodeStatus {}

export interface NodeStatus {
  readonly meshName: string;
  readonly channelName: string;
  readonly calledBy: string;
  readonly uptime: string;
  readonly processId: number;
}
// --8<-- [end:node-direct-contracts]

// The packet names handlers are registered under and messages are dispatched
// by. They match the class names above, which is what the caller side derives.
export const PacketNames = {
  getPlayerProfile: 'GetPlayerProfile',
  recordLogin: 'RecordLogin',
  issueSessionTicket: 'IssueSessionTicket',
  maintenanceNotice: 'MaintenanceNotice',
  getNodeStatus: 'GetNodeStatus',
  postChat: 'PostChat',
  getRoomState: 'GetRoomState',
  joinMatchQueue: 'JoinMatchQueue',
  changeNickname: 'ChangeNickname',
  getPlayer: 'GetPlayer',
  ping: 'Ping',
  pong: 'Pong',
  authenticate: 'Authenticate',
  nicknameChanged: 'NicknameChanged'
} as const;

// The mesh, channel, and node names both processes have to agree on.
export const TutorialNames = {
  mesh: 'game',
  profileChannel: 'profile',
  ticketingChannel: 'ticketing',
  broadcastChannel: 'broadcast',
  serverRoutingId: 'game-server-1',
  // The stable type a caller names when opening a room.
  gameRoomType: 'game-room',
  // The stable type a caller names when addressing a match queue.
  matchQueueType: 'match-queue',
  // The stable type a caller names when creating a player.
  playerActorType: 'player',
  // The stream node external clients connect to.
  clientStreamNode: 'client-stream'
} as const;
