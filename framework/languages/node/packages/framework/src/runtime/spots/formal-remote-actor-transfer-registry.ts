import type { RoutingId, ZLinkActor } from '../../contracts';
import type { ZLinkDeferredJoinAcceptedRoot } from '../actors';
import type { ZLinkActorHandoffPacket } from '../actors/actor-handoff';
import { ZLinkStateLane } from '../execution/state-lane';

export interface ZLinkFormalRemoteActorTransfer {
  readonly actor: ZLinkActor;
  readonly spotId: RoutingId;
  readonly transferId: string;
  readonly handoffBacklog: readonly ZLinkActorHandoffPacket[];
  readonly deferredJoinRoot?: ZLinkDeferredJoinAcceptedRoot;
  readonly targetLifecycleCompleted: Promise<void>;
  readonly sourceLeaveSubmitted: Promise<boolean>;
}

/** Keeps the source-leave fence attached to one admitted formal transfer. */
export class ZLinkFormalRemoteActorTransferRegistry {
  private readonly lane = new ZLinkStateLane();
  private readonly transfers = new Map<string, {
    readonly transfer: ZLinkFormalRemoteActorTransfer;
    readonly resolveTargetLifecycleCompleted: () => void;
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
    let resolveTargetLifecycleCompleted!: () => void;
    const targetLifecycleCompleted = new Promise<void>((resolve) => {
      resolveTargetLifecycleCompleted = resolve;
    });
    const sourceLeaveSubmitted = new Promise<boolean>((resolve) => {
      resolveSourceLeaveTerminal = resolve;
    });
    const transfer: ZLinkFormalRemoteActorTransfer = {
      ...input,
      targetLifecycleCompleted,
      sourceLeaveSubmitted
    };
    this.transfers.set(input.actor.context.actorId, {
      transfer,
      resolveTargetLifecycleCompleted,
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

  completeTargetLifecycle(actorId: string, transferId: string): boolean {
    const pending = this.transfers.get(actorId);
    if (pending === undefined || pending.transfer.transferId !== transferId) {
      return false;
    }
    pending.resolveTargetLifecycleCompleted();
    return true;
  }

  async completeSourceLeaveTerminal(
    actorId: string,
    transferId: string,
    succeeded: boolean
  ): Promise<boolean> {
    // The synchronous lookup remains in the caller's JS turn. Only the
    // target-lifecycle wait crosses turns, so the completion must re-check
    // the exact entry after that external await.
    const pending = this.completeSourceLeaveTerminalStartCore(actorId, transferId);
    if (pending === undefined) return false;
    await pending.transfer.targetLifecycleCompleted;
    return await this.lane.run(() => this.completeSourceLeaveTerminalCore(
      actorId,
      transferId,
      pending,
      succeeded
    ));
  }

  private completeSourceLeaveTerminalStartCore(actorId: string, transferId: string): {
    readonly transfer: ZLinkFormalRemoteActorTransfer;
    readonly resolveTargetLifecycleCompleted: () => void;
    readonly resolveSourceLeaveTerminal: (succeeded: boolean) => void;
  } | undefined {
    const pending = this.transfers.get(actorId);
    return pending === undefined || pending.transfer.transferId !== transferId
      ? undefined
      : pending;
  }

  private completeSourceLeaveTerminalCore(
    actorId: string,
    transferId: string,
    pending: {
      readonly transfer: ZLinkFormalRemoteActorTransfer;
      readonly resolveTargetLifecycleCompleted: () => void;
      readonly resolveSourceLeaveTerminal: (succeeded: boolean) => void;
    },
    succeeded: boolean
  ): boolean {
    if (
      pending.transfer.transferId !== transferId
      || this.transfers.get(actorId) !== pending
    ) return false;
    pending.resolveSourceLeaveTerminal(succeeded);
    return true;
  }
}
