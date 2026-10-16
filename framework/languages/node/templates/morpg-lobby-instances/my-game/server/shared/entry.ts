import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import {
  ZLinkSpotCreateState,
  type ZLinkEntrySpot,
  type ZLinkEntrySpotContext,
  type ZLinkSpotManager
} from '@zlink-systems/framework';
import { Names, Rejection } from './constants';
import { ActionRejected, PartyJoin } from './messages';
import { Player } from './player';
@Injectable({ scope: Scope.TRANSIENT })
export class Entry implements ZLinkEntrySpot<Player> {
  readonly context!: ZLinkEntrySpotContext<Player, Entry>;
  constructor(@Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager) {}
  async onJoinedActor(_actor: Player): Promise<void> {}
  async onLeaveActor(_actor: Player): Promise<void> {}
  async join(actor: Player, partyId: string, create: boolean): Promise<void> {
    if (!partyId.trim()) {
      await actor.context.boundSession.send(new ActionRejected(Rejection.invalidParty)).submit();
      return;
    }
    const id = Names.partyPrefix + partyId;
    const result = await this.spots.getOrCreate(id, Names.party).inMesh(Names.mesh).submit();
    if (result.state === ZLinkSpotCreateState.Rejected) throw new Error('Party creation rejected');
    actor.context.joinSpot(id, new PartyJoin(partyId, create)).defer();
  }
}
