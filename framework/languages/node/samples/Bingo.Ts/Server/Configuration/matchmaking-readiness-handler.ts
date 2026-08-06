import { Inject, Injectable, type OnApplicationBootstrap, type OnApplicationShutdown } from '@nestjs/common';
import type { ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import { ZLINK_ROUTE_MESH_RUNTIME } from '@zlink-systems/nestjs';
import { SampleNames } from './sample-names';

@Injectable()
class MatchmakingReadinessHandler implements OnApplicationBootstrap, OnApplicationShutdown {
  private readonly stop = new AbortController();

  constructor(@Inject(ZLINK_ROUTE_MESH_RUNTIME) private readonly runtime: ZLinkRouteMeshRuntime) {}

  onApplicationBootstrap(): void {
    void this.observeReadiness();
  }

  onApplicationShutdown(): void {
    this.stop.abort();
  }

  private async observeReadiness(): Promise<void> {
    this.logStatus(this.runtime.snapshot(SampleNames.matchmakingMeshName));
    for await (const observed of this.runtime.observe(
      SampleNames.matchmakingMeshName,
      64,
      this.stop.signal
    )) {
      this.logStatus(observed.status);
    }
  }

  private logStatus(status: ReturnType<ZLinkRouteMeshRuntime['snapshot']>): void {
    console.log(
      `bingo-matchmaking-status state=${status.state} readyPeers=${status.readyPeerCount}`
      + ` placement=${status.placement.isAvailable}`
    );
  }
}

export { MatchmakingReadinessHandler };
