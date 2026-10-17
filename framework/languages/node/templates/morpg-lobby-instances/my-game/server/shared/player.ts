import { Injectable } from '@nestjs/common';
import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorFactory,
  ZLinkActorJoinCompletion,
  ZLinkActorRelocationAdapter
} from '@zlink-systems/framework';
import { Rejection } from './constants';
import { ActionRejected, restoreNotification, type GameNotification } from './messages';
export class Player implements ZLinkActor {
  constructor(readonly context: ZLinkActorContext) {}
  async deliver(
    sourceSpotId: string,
    packetName: string,
    message: GameNotification
  ): Promise<void> {
    if (this.context.spotId !== sourceSpotId) return;
    await this.context.boundSession.send(restoreNotification(packetName, message)).submit();
  }
  async onJoinCompleted(completion: ZLinkActorJoinCompletion): Promise<void> {
    if (completion.status === 'rejected')
      await this.context.boundSession.send(new ActionRejected(Rejection.joinRejected)).submit();
    else if (completion.status !== 'accepted')
      throw new Error(`Spot join failed: ${completion.status}`);
  }
}
@Injectable()
export class PlayerFactory implements ZLinkActorFactory<Player> {
  async create(context: ZLinkActorContext): Promise<Player> {
    return new Player(context);
  }
}
// Identity and binding are Framework-owned; Player has no application state.
@Injectable()
export class PlayerRelocation implements ZLinkActorRelocationAdapter<Player> {
  async capture(_actor: Player): Promise<Uint8Array> {
    return new Uint8Array();
  }
  async restore(_actor: Player, _payload: Uint8Array): Promise<void> {}
}
