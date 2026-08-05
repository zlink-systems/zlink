import type { RoutingId, ZLinkActor } from '../../contracts';
import type { ZLinkDeferredJoinAcceptedRoot } from '../actors';
import type { ZLinkActorHandoffPacket } from '../actors/actor-handoff';

export interface ZLinkFormalRemoteActorTransfer {
  readonly actor: ZLinkActor;
  readonly spotId: RoutingId;
  readonly transferId: string;
  readonly handoffBacklog: readonly ZLinkActorHandoffPacket[];
  readonly deferredJoinRoot?: ZLinkDeferredJoinAcceptedRoot;
  readonly sourceLeaveTerminal: Promise<boolean>;
}

/** Keeps the source-leave fence attached to one admitted formal transfer. */
export class ZLinkFormalRemoteActorTransferRegistry {
  private readonly transfers = new Map<string, {
    readonly transfer: ZLinkFormalRemoteActorTransfer;
    readonly resolveSourceLeaveTerminal: (succeeded: boolean) => void;
  }>();
  private readonly transfersById = new Map<string, ZLinkFormalRemoteActorTransfer>();

  has(actorId: string): boolean {
    return this.transfers.has(actorId);
  }

  get(actorId: string): ZLinkFormalRemoteActorTransfer | undefined {
    return this.transfers.get(actorId)?.transfer;
  }

  getByTransferId(transferId: string): ZLinkFormalRemoteActorTransfer | undefined {
    return this.transfersById.get(transferId);
  }

  begin(input: {
    readonly actor: ZLinkActor;
    readonly spotId: RoutingId;
    readonly transferId: string;
    readonly handoffBacklog: readonly ZLinkActorHandoffPacket[];
    readonly deferredJoinRoot?: ZLinkDeferredJoinAcceptedRoot;
  }): ZLinkFormalRemoteActorTransfer {
    const existingByActor = this.transfers.get(input.actor.context.actorId)?.transfer;
    if (existingByActor !== undefined) {
      if (existingByActor.transferId !== input.transferId) {
        throw new Error(
          `Actor '${input.actor.context.actorId}' already has a different formal transfer.`
        );
      }
      return existingByActor;
    }
    const existingById = this.transfersById.get(input.transferId);
    if (existingById !== undefined) {
      if (existingById.actor.context.actorId !== input.actor.context.actorId) {
        throw new Error(`Formal actor transfer '${input.transferId}' does not match its Actor.`);
      }
      return existingById;
    }
    let resolveSourceLeaveTerminal!: (succeeded: boolean) => void;
    const sourceLeaveTerminal = new Promise<boolean>((resolve) => {
      resolveSourceLeaveTerminal = resolve;
    });
    const transfer: ZLinkFormalRemoteActorTransfer = {
      ...input,
      sourceLeaveTerminal
    };
    this.transfers.set(input.actor.context.actorId, {
      transfer,
      resolveSourceLeaveTerminal
    });
    this.transfersById.set(input.transferId, transfer);
    return transfer;
  }

  delete(actorId: string): void {
    const pending = this.transfers.get(actorId);
    if (pending !== undefined) {
      this.transfersById.delete(pending.transfer.transferId);
      this.transfers.delete(actorId);
    }
  }

  completeSourceLeaveTerminal(
    actorId: string,
    transferId: string,
    succeeded: boolean
  ): boolean {
    const pending = this.transfers.get(actorId);
    if (pending === undefined || pending.transfer.transferId !== transferId) {
      return false;
    }
    pending.resolveSourceLeaveTerminal(succeeded);
    return true;
  }
}
