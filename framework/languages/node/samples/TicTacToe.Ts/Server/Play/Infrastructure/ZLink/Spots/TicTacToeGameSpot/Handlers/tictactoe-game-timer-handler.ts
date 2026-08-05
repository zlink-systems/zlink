import type {
  ZLinkSpotTimerHandler,
  ZLinkTimerTick
} from '@zlink-systems/framework';
import { zlinkSpotTimerHandler } from '@zlink-systems/nestjs';
import { ZLinkTimerOverrunPolicy } from '@zlink-systems/framework';
import { TicTacToeGameSpot } from '../tictactoe-game-spot';

@zlinkSpotTimerHandler({
  spot: () => TicTacToeGameSpot,
  name: 'game-tick',
  periodMs: 1000,
  options: {
    overrunPolicy: ZLinkTimerOverrunPolicy.DelayNextTick,
    stopOnUnhandledException: true
  }
})
// --8<-- [start:doc-timer-handler]
class TicTacToeGameTimerHandler implements ZLinkSpotTimerHandler<TicTacToeGameSpot> {
  async handle(spot: TicTacToeGameSpot, tick: ZLinkTimerTick): Promise<void> {
    void tick;
    await spot.tick();
  }
}
// --8<-- [end:doc-timer-handler]

export { TicTacToeGameTimerHandler };
