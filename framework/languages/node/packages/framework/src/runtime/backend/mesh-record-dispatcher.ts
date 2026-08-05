import {
  ReadyOwnerKind,
  ReceiveKind,
  type ReadyRecord,
  type ReceiveRecord
} from '../foundation/service-runtime-contracts';

export interface ZLinkMeshRecordDispatchPorts {
  readonly node: (record: ReceiveRecord) => void | Promise<void>;
  readonly spot: (owner: ReadyRecord, record: ReceiveRecord) => void | Promise<void>;
  readonly actor: (owner: ReadyRecord, record: ReceiveRecord) => void | Promise<void>;
  readonly completion: (record: ReceiveRecord) => void | Promise<void>;
  readonly sendReady: (record: ReceiveRecord) => void | Promise<void>;
  readonly transferControl: (record: ReceiveRecord) => void | Promise<void>;
}

/**
 * Routes one pulled Core record to the framework component that owns its
 * lifecycle. The pump retains the record until the returned promise settles.
 */
export class ZLinkMeshRecordDispatcher {
  constructor(private readonly ports: ZLinkMeshRecordDispatchPorts) {}

  dispatch(owner: ReadyRecord, record: ReceiveRecord): void | Promise<void> {
    switch (record.kind) {
      case ReceiveKind.NodeSend:
      case ReceiveKind.NodeRequest:
      case ReceiveKind.ChannelSend:
      case ReceiveKind.ChannelRequest:
        this.requireOwner(owner, ReadyOwnerKind.Node, record);
        return this.ports.node(record);
      case ReceiveKind.SpotSend:
      case ReceiveKind.SpotRequest:
      case ReceiveKind.SpotMulticast:
      case ReceiveKind.SpotControl:
        this.requireOwner(owner, ReadyOwnerKind.Spot, record);
        return this.ports.spot(owner, record);
      case ReceiveKind.ActorSend:
      case ReceiveKind.ActorRequest:
        this.requireOwner(owner, ReadyOwnerKind.Actor, record);
        return this.ports.actor(owner, record);
      case ReceiveKind.Completion:
        return this.ports.completion(record);
      case ReceiveKind.SendReady:
        return this.ports.sendReady(record);
      case ReceiveKind.TransferControl:
        return this.ports.transferControl(record);
      default:
        throw new Error(`Unsupported MeshNode receive kind '${record.kind}'.`);
    }
  }

  private requireOwner(owner: ReadyRecord, expected: number, record: ReceiveRecord): void {
    if (owner.ownerKind !== expected) {
      throw new Error(
        `MeshNode receive kind '${record.kind}' requires owner kind '${expected}', got '${owner.ownerKind}'.`
      );
    }
  }
}
