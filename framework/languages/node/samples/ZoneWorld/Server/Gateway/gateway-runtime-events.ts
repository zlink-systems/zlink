import { Inject, Injectable, type OnApplicationBootstrap, type OnApplicationShutdown } from '@nestjs/common';
import type { ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import { ZLINK_ROUTE_MESH_RUNTIME } from '@zlink-systems/nestjs';
import { ZoneWorldNames } from '../../Shared/spec';

@Injectable()
class GatewaySpotEventHandler implements OnApplicationBootstrap, OnApplicationShutdown {
  private readonly stop = new AbortController();

  constructor(@Inject(ZLINK_ROUTE_MESH_RUNTIME) private readonly runtime: ZLinkRouteMeshRuntime) {}

  onApplicationBootstrap(): void {
    void this.observeReadiness();
  }

  onApplicationShutdown(): void {
    this.stop.abort();
  }

  private async observeReadiness(): Promise<void> {
    this.logStatus(this.runtime.snapshot(ZoneWorldNames.zoneMesh));
    for await (const observed of this.runtime.observe(ZoneWorldNames.zoneMesh, 64, this.stop.signal)) {
      this.logStatus(observed.status);
    }
  }

  private logStatus(status: import('@zlink-systems/framework').ZLinkRouteMeshStatus): void {
    console.log(
      `gateway mesh status mesh=${status.meshName} state=${status.state}`
      + ` readyPeers=${status.readyPeerCount}`
    );
  }
}

export { GatewaySpotEventHandler };
