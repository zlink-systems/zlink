import type {
  ZLinkSpotTimerHandler,
  ZLinkTimerTick
} from '@zlink-systems/framework';
import { Injectable } from '@nestjs/common';
import { TicTacToeGameSpot } from '../tictactoe-game-spot';

@Injectable()
// --8<-- [start:doc-timer-handler]
class TicTacToeGameTimerHandler implements ZLinkSpotTimerHandler<TicTacToeGameSpot> {
  async handle(spot: TicTacToeGameSpot, tick: ZLinkTimerTick): Promise<void> {
    void tick;
    await spot.tick();
  }
}
// --8<-- [end:doc-timer-handler]

export { TicTacToeGameTimerHandler };
