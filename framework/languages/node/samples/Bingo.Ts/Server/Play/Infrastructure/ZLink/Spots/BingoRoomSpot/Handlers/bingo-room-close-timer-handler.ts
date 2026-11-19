import { Injectable } from '@nestjs/common';
import type { ZLinkSpotTimerHandler, ZLinkTimerTick } from '@zlink-systems/framework';
import type { BingoRoomSpot } from '../bingo-room-spot';

@Injectable()
class BingoRoomCloseTimerHandler implements ZLinkSpotTimerHandler<BingoRoomSpot> {
  async handle(spot: BingoRoomSpot, tick: ZLinkTimerTick): Promise<void> {
    void tick;
    if (!spot.tryRequestClose()) return;
    console.error(`bingo-lifecycle room-close spot=${spot.context.spotId}`);
    void spot.context.close();
  }
}

export { BingoRoomCloseTimerHandler };
