import { Injectable, Scope } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type {
  ZLinkMessage,
  ZLinkSpot,
  ZLinkSpotActorJoinResult,
  ZLinkSpotContext,
  ZLinkSpotCreateResponse,
  ZLinkSpotPacketHandler,
  ZLinkSpotRequestHandler
} from '@zlink-systems/framework';
import {
  PacketNames,
  type GetRoomState,
  type OpenRoom,
  type PostChat,
  type RoomState
} from '../../Shared/contracts';

// --8<-- [start:spot-class]
// A room owns its own state and is addressed by a global SpotId. Messages sent
// to one room run one at a time, so the fields below need no synchronization.
//
// Every Node user Spot names the actor type it can admit. This room admits
// none, so it takes the default type and rejects every join. Actors are a later
// chapter. The scope is TRANSIENT because the Framework builds one instance per
// room.
@Injectable({ scope: Scope.TRANSIENT })
class GameRoom implements ZLinkSpot {
  readonly context!: ZLinkSpotContext;

  private title = 'untitled';
  private readonly chat: string[] = [];

  // Runs before the room accepts any message. Rejecting here means the create
  // call fails and no room exists. Omit this method to accept every request.
  async onCreate(request: ZLinkMessage): Promise<ZLinkSpotCreateResponse> {
    const body = request.decode<OpenRoom>();
    this.title = body.title;
    return { accepted: true };
  }

  append(line: string): void {
    this.chat.push(line);
  }

  state(): RoomState {
    return {
      title: this.title,
      chat: [...this.chat]
    };
  }

  // No actor ever joins this room, so the three membership callbacks below say
  // so and do nothing else.
  async onActorJoin(): Promise<ZLinkSpotActorJoinResult> {
    return { accepted: false };
  }

  async onJoinedActor(): Promise<void> {}

  async onLeaveActor(): Promise<void> {}
}
// --8<-- [end:spot-class]

// --8<-- [start:spot-handlers]
// Spot handlers live in their own classes and take the target room as the first
// argument. The decorator names the room type and the packet, so nothing has to
// be registered again in the module.
@zlinkSpotPacketHandler({
  spot: () => GameRoom,
  packetName: PacketNames.postChat
})
class PostChatHandler implements ZLinkSpotPacketHandler<GameRoom, PostChat> {
  async handle(room: GameRoom, message: PostChat): Promise<void> {
    const line = `${message.playerId}: ${message.text}`;
    room.append(line);
  }
}

// The return value is the reply. This handler only reads.
@zlinkSpotPacketHandler({
  spot: () => GameRoom,
  packetName: PacketNames.getRoomState
})
class GetRoomStateHandler implements ZLinkSpotRequestHandler<GameRoom, GetRoomState, RoomState> {
  async handle(room: GameRoom): Promise<RoomState> {
    return room.state();
  }
}
// --8<-- [end:spot-handlers]

export { GameRoom, GetRoomStateHandler, PostChatHandler };
