import type { ActorRef } from '../../contracts';
import type { ZLinkBackendActorSessionNode } from '../backend';
import {
  type ZLinkActorRoutedJoinTransport
} from '../actors';
import type { DefaultZLinkActorManager } from '../actors';
import type { DefaultZLinkSpotManager, ZLinkSpotNodeRuntimeManager } from '../spots';
import type { ZLinkDetachedTaskRunner } from '../spots/spot-actor-join-dispatch';
import type {
  ZLinkBoundSessionResponsePort,
  ZLinkRemoteBoundSessionPort,
  ZLinkStreamActorLookupPort
} from '../streams/stream-binding-runtime-ports';
import type { DefaultZLinkBoundSession } from '../streams/session-context';
import type { MeshRouterResolver } from './mesh-router-resolver';
import type { ZLinkStoreLocationResolvers } from '../locations';
import { ZLinkRemoteBoundSessionRelay } from './remote-bound-session-relay';
import { ZLinkActorPacketRelay } from './actor-packet-relay';
import { ZLinkRemoteActorJoinReceiver } from './remote-actor-join-receiver';

export interface ZLinkBoundSessionRelayOptions {
  readonly requestTimeoutMs?: number;
  readonly routeTransport: ZLinkActorRoutedJoinTransport;
  readonly streamBindingRuntime: () =>
    ZLinkBoundSessionResponsePort & ZLinkRemoteBoundSessionPort & ZLinkStreamActorLookupPort;
  readonly meshRouters: MeshRouterResolver;
  readonly actorManager: () => DefaultZLinkActorManager | undefined;
  readonly spotManager: () => DefaultZLinkSpotManager | undefined;
  readonly spotNodeRuntime: () => ZLinkSpotNodeRuntimeManager | undefined;
  readonly detachedTaskRunner: ZLinkDetachedTaskRunner;
  readonly actorSessionNode: (actorId: string) => ZLinkBackendActorSessionNode | undefined;
  readonly actorLocationResolver?: () => ZLinkStoreLocationResolvers | undefined;
  readonly destroyedActorRefs: ReadonlyMap<string, ActorRef>;
  readonly errorSink: () => { reportRuntimeTaskException(taskName: string, error: unknown): void };
  readonly boundSessionFactory: (actorId: string) => DefaultZLinkBoundSession;
}

export class ZLinkBoundSessionRelay {
  readonly actorPackets: ZLinkActorPacketRelay;
  readonly boundSessions: ZLinkRemoteBoundSessionRelay;
  readonly actorJoins: ZLinkRemoteActorJoinReceiver;

  constructor(options: ZLinkBoundSessionRelayOptions) {
    this.actorPackets = new ZLinkActorPacketRelay({
      requestTimeoutMs: options.requestTimeoutMs,
      routeTransport: options.routeTransport,
      actorManager: options.actorManager,
      streamBindingRuntime: options.streamBindingRuntime,
      meshRouters: options.meshRouters,
      spotManager: options.spotManager,
      spotNodeRuntime: options.spotNodeRuntime,
      detachedTaskRunner: options.detachedTaskRunner,
      errorSink: options.errorSink,
      actorLocationResolver: options.actorLocationResolver
    });
    this.boundSessions = new ZLinkRemoteBoundSessionRelay({
      requestTimeoutMs: options.requestTimeoutMs,
      routeTransport: options.routeTransport,
      streamBindingRuntime: options.streamBindingRuntime,
      actorManager: options.actorManager,
      meshRouters: options.meshRouters,
      actorSessionNode: options.actorSessionNode,
      destroyedActorRefs: options.destroyedActorRefs,
      boundSessionFactory: options.boundSessionFactory,
      updateRemoteActorPacketTarget: (actorId, value) =>
        this.actorPackets.updateRemoteActorPacketTarget(actorId, value),
      actorPacketTargetForState: (actorId, routerChannelIdHint) =>
        this.actorPackets.actorPacketTargetForState(actorId, routerChannelIdHint),
      reportOwnershipRefreshError: (actorId, error) =>
        options.errorSink().reportRuntimeTaskException(
          `bound session ownership refresh for '${actorId}'`,
          error
        )
    });
    this.actorJoins = new ZLinkRemoteActorJoinReceiver({
      actorManager: options.actorManager,
      spotManager: options.spotManager
    });
  }

  clearRemoteActorPacketTarget(actorId: string): void {
    this.actorPackets.clearRemoteActorPacketTarget(actorId);
    this.boundSessions.clearOwnership(actorId);
  }
}
