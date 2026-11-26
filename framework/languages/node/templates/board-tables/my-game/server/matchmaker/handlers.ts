import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import { PacketNames } from '../shared/constants';
import type {
  CancelTicket,
  EnterReport,
  MatchCancelled,
  MatchPresence,
  MatchQueued,
  MatchRequest,
  ReturnReport,
  RoomClosed,
  RoomPrepared,
  RoomWithdrawn
} from '../shared/messages';
import { Pool } from './pool';

@zlinkSpotPacketHandler({ spot: () => Pool, packetName: PacketNames.matchRequest })
export class QueuePlayer {
  async handle(spot: Pool, request: MatchRequest): Promise<MatchQueued> {
    return spot.enqueue(request);
  }
}

@zlinkSpotPacketHandler({ spot: () => Pool, packetName: PacketNames.cancelTicket })
export class CancelPlayer {
  async handle(spot: Pool, request: CancelTicket): Promise<MatchCancelled> {
    return spot.cancel(request);
  }
}

@zlinkSpotPacketHandler({ spot: () => Pool, packetName: PacketNames.matchPresence })
export class UpdatePresence {
  async handle(spot: Pool, message: MatchPresence): Promise<void> {
    await spot.presence(message);
  }
}

@zlinkSpotPacketHandler({ spot: () => Pool, packetName: PacketNames.roomWithdrawn })
export class ConfirmWithdrawal {
  async handle(spot: Pool, message: RoomWithdrawn): Promise<void> {
    await spot.roomWithdrawn(message);
  }
}

@zlinkSpotPacketHandler({ spot: () => Pool, packetName: PacketNames.enterReport })
export class ReportEntry {
  async handle(spot: Pool, report: EnterReport): Promise<void> {
    await spot.report(report);
  }
}

@zlinkSpotPacketHandler({ spot: () => Pool, packetName: PacketNames.returnReport })
export class ReportReturn {
  async handle(spot: Pool, report: ReturnReport): Promise<void> {
    await spot.returned(report);
  }
}

@zlinkSpotPacketHandler({ spot: () => Pool, packetName: PacketNames.roomPrepared })
export class ReportPreparation {
  async handle(spot: Pool, message: RoomPrepared): Promise<void> {
    await spot.roomPrepared(message);
  }
}

@zlinkSpotPacketHandler({ spot: () => Pool, packetName: PacketNames.roomClosed })
export class ReportRoomClosure {
  async handle(spot: Pool, message: RoomClosed): Promise<void> {
    spot.roomClosed(message);
  }
}
