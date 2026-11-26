import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_SPOT_MANAGER, zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import {
  ZLinkSpotCreateState,
  type ZLinkInstanceSpot,
  type ZLinkInstanceSpotContext,
  type ZLinkSpotManager
} from '@zlink-systems/framework';
import { Names, PacketNames } from '../../shared/constants';
import { RoomClosed, RoomPrepared, PrepareRoom, type WithdrawRoom } from '../../shared/messages';

@Injectable({ scope: Scope.TRANSIENT })
export class RoomProvisioner implements ZLinkInstanceSpot {
  readonly context!: ZLinkInstanceSpotContext;

  constructor(@Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager) {}

  async prepare(message: PrepareRoom): Promise<void> {
    const id = message.assignmentId;
    try {
      const created = await this.spots.getOrCreate(id, Names.table).inMesh(Names.mesh).submit();
      if (created.state !== ZLinkSpotCreateState.Rejected) {
        await this.context.outbound.sendToSpot(id, new PrepareRoom(id, message.members)).submit();
        return;
      }
    } catch (error) {
      console.error('Room preparation failed', id, error);
    }
    await this.context.outbound
      .sendToSpot(Names.poolKey, new RoomPrepared(id, false))
      .instanceSpot(Names.pool)
      .inMesh(Names.mesh)
      .submit();
  }

  async close(id: string): Promise<void> {
    const room = await this.spots.find(id);
    if (room && !(await this.spots.close(room))) throw new Error(`Room still occupied: ${id}`);
    await this.context.outbound
      .sendToSpot(Names.poolKey, new RoomClosed(id))
      .instanceSpot(Names.pool)
      .inMesh(Names.mesh)
      .submit();
  }
}

@zlinkSpotPacketHandler({ spot: () => RoomProvisioner, packetName: PacketNames.prepareRoom })
export class ProvisionRoom {
  async handle(spot: RoomProvisioner, message: PrepareRoom): Promise<void> {
    await spot.prepare(message);
  }
}

@zlinkSpotPacketHandler({ spot: () => RoomProvisioner, packetName: PacketNames.withdrawRoom })
export class CloseRoom {
  async handle(spot: RoomProvisioner, message: WithdrawRoom): Promise<void> {
    await spot.close(message.assignmentId);
  }
}
