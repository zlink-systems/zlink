import { Injectable, Scope } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import { BingoMatchReservationStore } from './bingo-match-reservation-store';
import type {
  ZLinkInstanceSpot,
  ZLinkInstanceSpotContext,
  ZLinkSpotRequestHandler,
  ZLinkSpotTimerHandler,
  ZLinkTimerTick
} from '@zlink-systems/framework';
import type { ReserveBingoRoomReq, ReserveBingoRoomRes } from '../../Shared/Contracts/bingo-messages.generated';

@Injectable({ scope: Scope.TRANSIENT })
class BingoMatchmaker implements ZLinkInstanceSpot {
  readonly context!: ZLinkInstanceSpotContext;
  lastActivity = Date.now();

  async onInitialize(): Promise<void> {
    await this.context.addTimer('bingo-matchmaker-idle', 10_000, BingoMatchmakerIdleTimer);
  }
}

@Injectable()
@zlinkSpotPacketHandler({ spot: () => BingoMatchmaker, packetName: 'ReserveBingoRoomReq' })
class ReserveBingoRoomHandler
  implements ZLinkSpotRequestHandler<BingoMatchmaker, ReserveBingoRoomReq, ReserveBingoRoomRes> {
  constructor(private readonly reservations: BingoMatchReservationStore) {}

  async handle(spot: BingoMatchmaker, request: ReserveBingoRoomReq): Promise<ReserveBingoRoomRes> {
    spot.lastActivity = Date.now();
    return await this.reservations.reserve(request);
  }
}

class BingoMatchmakerIdleTimer implements ZLinkSpotTimerHandler<BingoMatchmaker> {
  async handle(spot: BingoMatchmaker, _tick: ZLinkTimerTick): Promise<void> {
    if (Date.now() - spot.lastActivity >= 30_000) await spot.context.close();
  }
}

export { BingoMatchmaker, BingoMatchmakerIdleTimer, ReserveBingoRoomHandler };
