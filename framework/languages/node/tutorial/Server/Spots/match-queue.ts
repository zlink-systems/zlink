import { Injectable, Scope } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type {
  ZLinkInstanceSpot,
  ZLinkInstanceSpotContext,
  ZLinkSpotRequestHandler
} from '@zlink-systems/framework';
import { PacketNames, type JoinMatchQueue, type MatchQueueStatus } from '../../Shared/contracts';

// --8<-- [start:instance-spot-class]
// Unlike a room, a match queue is never created explicitly. The first message
// addressed to a queue id brings it into being and is then handled by it.
// Players do not join it as members; it only processes requests, so there is
// no actor type to name and no create or join callback to write. The scope is
// TRANSIENT because the Framework builds one instance per queue.
@Injectable({ scope: Scope.TRANSIENT })
class MatchQueue implements ZLinkInstanceSpot {
  readonly context!: ZLinkInstanceSpotContext;

  private readonly waiting: string[] = [];

  get waitingCount(): number {
    return this.waiting.length;
  }

  enqueue(playerId: string): void {
    this.waiting.push(playerId);
  }
}
// --8<-- [end:instance-spot-class]

// --8<-- [start:instance-spot-handler]
// Handlers are written the same way as room
// handlers: the decorator names the queue type
// and the packet, and the return value is the
// reply.
@zlinkSpotPacketHandler({
  spot: () => MatchQueue,
  packetName: PacketNames.joinMatchQueue
})
class JoinMatchQueueHandler implements ZLinkSpotRequestHandler<
  MatchQueue,
  JoinMatchQueue,
  MatchQueueStatus
> {
  async handle(queue: MatchQueue, request: JoinMatchQueue): Promise<MatchQueueStatus> {
    queue.enqueue(request.playerId);
    return { waiting: queue.waitingCount };
  }
}
// --8<-- [end:instance-spot-handler]

export { JoinMatchQueueHandler, MatchQueue };
