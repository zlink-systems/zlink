import { Injectable } from '@nestjs/common';
import type { ZLinkSpotTimerHandler, ZLinkTimerTick } from '@zlink-systems/framework';
import type { TicTacToeGameSpot } from '../tictactoe-game-spot';

@Injectable()
class TicTacToeGameCloseTimerHandler implements ZLinkSpotTimerHandler<TicTacToeGameSpot> {
  async handle(spot: TicTacToeGameSpot, tick: ZLinkTimerTick): Promise<void> {
    void tick;
    if (!spot.tryRequestClose()) return;
    console.error(`tictactoe-lifecycle game-close spot=${spot.context.spotId}`);
    void spot.context.close();
  }
}

export { TicTacToeGameCloseTimerHandler };
