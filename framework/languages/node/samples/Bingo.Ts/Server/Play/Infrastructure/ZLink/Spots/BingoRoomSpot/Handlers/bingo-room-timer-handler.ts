import { zlinkSpotTimerHandler } from '@zlink-systems/nestjs';
import { ZLinkTimerOverrunPolicy } from '@zlink-systems/framework';
import type { ZLinkSpotTimerHandler, ZLinkTimerTick } from '@zlink-systems/framework';
import { BingoRoomSpot } from '../bingo-room-spot';

// --8<-- [start:doc-bingo-draw-timer]
@zlinkSpotTimerHandler({
  spot: () => BingoRoomSpot,
  name: 'bingo-draw',
  periodMs: 200,
  options: {
    overrunPolicy: ZLinkTimerOverrunPolicy.DelayNextTick,
    stopOnUnhandledException: true
  }
})
class BingoRoomTimerHandler implements ZLinkSpotTimerHandler<BingoRoomSpot> {
  async handle(room: BingoRoomSpot, tick: ZLinkTimerTick): Promise<void> {
    void tick;
    await room.drawNextNumber();
  }
}
// --8<-- [end:doc-bingo-draw-timer]

export { BingoRoomTimerHandler };
