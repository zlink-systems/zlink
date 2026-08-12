import { Inject, Injectable } from '@nestjs/common';
import {
  ZLINK_ACTOR_CLIENT,
  zlinkEntrySpotActorSendHandler
} from '@zlink-systems/nestjs';
import { PlayerActor } from '../../Actors/player-actor';
import {
  DestroyBingoActorMsg,
  PlayerActorCreateReq
} from '../../../../../../Shared/Contracts/bingo-messages.generated';
import type {
  ZLinkActorClient,
  ZLinkActorCreateResponse,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessage,
  ZLinkSpotActorJoinResult,
  ZLinkMessageContext
} from '@zlink-systems/framework';

@Injectable()
class BingoEntrySpot implements ZLinkEntrySpot<PlayerActor> {
  readonly context!: ZLinkEntrySpotContext<PlayerActor>;

  constructor(
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient
  ) {}

  async onJoinedActor(actor: PlayerActor): Promise<void> {
    if (!actor.consumeDestroyAfterEntrySpotJoin()) {
      console.error(`bingo-lifecycle entry-joined actor=${actor.actorId} destroy=false`);
      return;
    }
    await this.actors
      .sendToActor(actor.actorId, new DestroyBingoActorMsg({}))
      .submit();
  }

  async onActorJoin(
    _actorId: string,
    _request: ZLinkMessage
  ): Promise<ZLinkSpotActorJoinResult> {
    return { accepted: true };
  }

  async onCreateActor(actor: PlayerActor, createRequest: ZLinkMessage): Promise<ZLinkActorCreateResponse> {
    const request = createRequest.decode<PlayerActorCreateReq>();
    actor.displayName = request.displayName;
    return { accepted: true };
  }

  async onLeaveActor(actor: PlayerActor): Promise<void> {
    console.error(`bingo-lifecycle entry-leave actor=${actor.actorId}`);
  }

  async onDisconnectActor(_actor: PlayerActor): Promise<void> {}

  scheduleDestroy(actor: PlayerActor): void {
    void this.context.runIoWorker(async () => true).submit().then(async () => {
      console.error(`bingo-lifecycle entry-destroy-start actor=${actor.actorId}`);
      await this.context.destroyActor(actor);
      console.error(`bingo-lifecycle entry-destroy-complete actor=${actor.actorId}`);
    });
  }
}

@zlinkEntrySpotActorSendHandler({
  actor: () => PlayerActor,
  entrySpot: () => BingoEntrySpot,
  packetName: 'DestroyBingoActorMsg'
})
class DestroyBingoActorMsgHandler {
  constructor(private readonly entrySpot: BingoEntrySpot) {}

  async handle(
    _spot: BingoEntrySpot,
    actor: PlayerActor,
    _context: ZLinkMessageContext,
    _message: DestroyBingoActorMsg
  ): Promise<void> {
    this.entrySpot.scheduleDestroy(actor);
  }
}

export { BingoEntrySpot, DestroyBingoActorMsgHandler };
