import { Inject, Injectable, type OnApplicationBootstrap, type OnApplicationShutdown } from '@nestjs/common';
import { ZLINK_ROUTE_MESH_RUNTIME } from '@zlink-systems/nestjs';
import { ZLinkPeerState, type ZLinkRouteMeshRuntime, type ZLinkRouteMeshStatus } from '@zlink-systems/framework';
import type { NodeView } from '../../Shared/contracts';
import { ZoneWorldNames } from '../../Shared/spec';
import { NodeRegistry } from './node-registry';
import { OpsConsoleRegistry } from './ops-console-registry';

@Injectable()
class OpsRuntimeStatusObserver implements OnApplicationBootstrap, OnApplicationShutdown {
  private readonly stop = new AbortController();

  constructor(
    @Inject(ZLINK_ROUTE_MESH_RUNTIME) private readonly routeMeshRuntime: ZLinkRouteMeshRuntime,
    private readonly nodes: NodeRegistry,
    private readonly consoles: OpsConsoleRegistry
  ) {}

  onApplicationBootstrap(): void {
    void this.observeReadiness();
  }

  onApplicationShutdown(): void {
    this.stop.abort();
  }

  private async observeReadiness(): Promise<void> {
    const initial = this.routeMeshRuntime.snapshot(ZoneWorldNames.zoneMesh);
    this.publish(this.nodes.applyLiveRoutingIds(this.liveRoutingIds(initial)));
    for await (const observed of this.routeMeshRuntime.observe(
      ZoneWorldNames.zoneMesh,
      64,
      this.stop.signal
    )) {
      this.publish(this.nodes.applyLiveRoutingIds(this.liveRoutingIds(observed.status)));
    }
  }

  private liveRoutingIds(status: ZLinkRouteMeshStatus): ReadonlySet<string> {
    return new Set(
      status.peers
        .filter((peer) => peer.state === ZLinkPeerState.Ready)
        .map((peer) => peer.nodeRid)
    );
  }

  private publish(nodes: readonly NodeView[]): void {
    for (const node of nodes) this.consoles.publish(node);
  }
}

export { OpsRuntimeStatusObserver };
