import { Inject, Injectable } from '@nestjs/common';
import {
  ZLINK_ACTOR_CLIENT,
  zlinkEntrySpotActorSendHandler
} from '@zlink-systems/nestjs';
import { PlayerActor } from '../../Actors/player-actor';
import { PendingBingoActorDestroyRegistry } from '../../Actors/player-actor-lifecycle-handlers';
import {
  DestroyBingoActor,
  EnsurePlayerActorReq as GeneratedEnsurePlayerActorReq
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
    private readonly pendingDestroys: PendingBingoActorDestroyRegistry,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient
  ) {}

  async onJoinedActor(actor: PlayerActor): Promise<void> {
    if (!this.pendingDestroys.consume(actor.actorId)) {
      console.error(`bingo-lifecycle entry-joined actor=${actor.actorId} destroy=false`);
      return;
    }
    await this.actors
      .sendToActor(actor.actorId, new DestroyBingoActor({}))
      .submit();
  }

  async onActorJoin(
    _actorId: string,
    _request: ZLinkMessage
  ): Promise<ZLinkSpotActorJoinResult> {
    return { accepted: true };
  }

  async onCreateActor(actor: PlayerActor, createRequest: ZLinkMessage): Promise<ZLinkActorCreateResponse> {
    const request = createRequest.decode<GeneratedEnsurePlayerActorReq>();
    if (typeof request.displayName === 'string') {
      await this.actors
        .sendToActor(actor.actorId, request)
        .submit();
    }
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
  packetName: 'DestroyBingoActor'
})
class DestroyBingoActorHandler {
  constructor(private readonly entrySpot: BingoEntrySpot) {}

  async handle(
    _spot: BingoEntrySpot,
    actor: PlayerActor,
    _context: ZLinkMessageContext,
    _message: DestroyBingoActor
  ): Promise<void> {
    this.entrySpot.scheduleDestroy(actor);
  }
}

export { BingoEntrySpot, DestroyBingoActorHandler };
