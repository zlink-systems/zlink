import type { ZLinkSpotRelocationAdapter } from '@zlink-systems/framework';
import { BingoRoomSpot } from './bingo-room-spot';

type BingoRoomRelocationState = ReturnType<BingoRoomSpot['captureRelocationState']>;

class BingoRoomRelocationAdapter implements ZLinkSpotRelocationAdapter<BingoRoomSpot> {
  // The Framework retains owner, queue, timer and accepted-journal state. The
  // adapter keeps only Bingo's domain state required to reconstruct the room.
  async capture(spot: BingoRoomSpot): Promise<Uint8Array> {
    return new TextEncoder().encode(JSON.stringify(spot.captureRelocationState()));
  }

  async restore(spot: BingoRoomSpot, payload: Uint8Array): Promise<void> {
    spot.restoreRelocationState(
      JSON.parse(new TextDecoder().decode(payload)) as BingoRoomRelocationState
    );
  }
}

export { BingoRoomRelocationAdapter };
