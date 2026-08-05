import type { RoutingId } from '../../contracts/Common';
import {
  ZLinkLocationAutoConnectType,
  ZLinkLocationRole,
  ZLinkLocationWriteIntent,
  type ZLinkLocationWriteResult,
  type ZLinkPeerLocation,
  type ZLinkPeerLocationKey
} from './internal-location-contracts';

export interface ZLinkAutoConnectLocal {
  readonly autoConnectType: ZLinkLocationAutoConnectType;
  readonly meshName: string;
  readonly role: ZLinkLocationRole;
  readonly nodeRid?: RoutingId;
  readonly endpoint: string;
  readonly objectRole?: 'none' | 'client' | 'server';
  readonly hasRouteMeshServerChannel?: boolean;
}

export interface ZLinkAutoConnectTarget {
  readonly targetKey: string;
  readonly nodeRid?: RoutingId;
  readonly lifecycleGeneration: bigint;
  readonly role: ZLinkLocationRole;
  readonly endpoint: string;
  readonly metadata?: Readonly<Record<string, string>>;
  readonly ownerId?: string;
  readonly descriptorRevision?: bigint;
}

export interface IZLinkAutoConnectExecutor {
  connect(target: ZLinkAutoConnectTarget): boolean;
  disconnect(target: ZLinkAutoConnectTarget): void;
  disconnectStalePeers?(targets: readonly ZLinkAutoConnectTarget[]): void;
  isDisconnected?(target: ZLinkAutoConnectTarget): boolean;
  onDisconnected?(handler: (endpoint: string) => void): void;
  replaceNotRequired?(targets: readonly ZLinkAutoConnectTarget[]): void;
}

export interface IZLinkAutoConnectPeerPublisher {
  writePeer(
    peer: ZLinkPeerLocation,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult>;
  removePeer(
    key: ZLinkPeerLocationKey,
    generation: bigint,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult>;
}

export interface ZLinkAutoConnectEventSink {
  desiredSetChanged(change: {
    readonly autoConnectType: ZLinkLocationAutoConnectType;
    readonly meshName: string;
    readonly connectedEndpoints: readonly string[];
    readonly disconnectedEndpoints: readonly string[];
  }): void;
}
