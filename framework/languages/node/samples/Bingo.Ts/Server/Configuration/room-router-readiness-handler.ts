import { Inject, Injectable, type OnApplicationBootstrap, type OnApplicationShutdown } from '@nestjs/common';
import { ZLinkPeerState, type ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import { ZLINK_ROUTE_MESH_RUNTIME } from '@zlink-systems/nestjs';
import { BINGO_SAMPLE_CONFIG, type BingoSampleConfig } from './sample-config';
import { SampleNames } from './sample-names';

@Injectable()
class RoomRouterReadinessHandler implements OnApplicationBootstrap, OnApplicationShutdown {
  private readonly stop = new AbortController();

  private readonly emitted = new Set<string>();

  constructor(
    @Inject(ZLINK_ROUTE_MESH_RUNTIME) private readonly routeRuntime: ZLinkRouteMeshRuntime,
    @Inject(BINGO_SAMPLE_CONFIG) private readonly config: BingoSampleConfig
  ) {}

  onApplicationBootstrap(): void {
    if (this.config.nodeId.startsWith('play-')) {
      void this.observePlayPeer();
    } else if (this.config.nodeId.startsWith('api-')) {
      void this.observeMesh(SampleNames.matchmakingMeshName, 'matchmaking');
      void this.observeMesh(SampleNames.roomSpotNode, 'room');
    } else if (this.config.nodeId.startsWith('session-')) {
      void this.observeMesh(SampleNames.roomSpotNode, 'room');
    }
  }

  onApplicationShutdown(): void {
    this.stop.abort();
  }

  private async observePlayPeer(): Promise<void> {
    const hasReadyPeer = (): boolean => this.routeRuntime.snapshot(SampleNames.roomSpotNode).peers
      .some((peer) => peer.state === ZLinkPeerState.Ready);
    if (hasReadyPeer()) {
      this.emit(`bingo-ready kind=peer-route node=${this.config.nodeId} peer=${this.config.peerNodeId}`);
      return;
    }
    const observations = this.routeRuntime.observe(SampleNames.roomSpotNode, 64, this.stop.signal);
    if (hasReadyPeer()) {
      this.emit(`bingo-ready kind=peer-route node=${this.config.nodeId} peer=${this.config.peerNodeId}`);
      return;
    }
    for await (const observed of observations) {
      if (!observed.status.peers.some((peer) => peer.state === ZLinkPeerState.Ready)) continue;
      this.emit(`bingo-ready kind=peer-route node=${this.config.nodeId} peer=${this.config.peerNodeId}`);
      return;
    }
  }

  private async observeMesh(meshName: string, logName: string): Promise<void> {
    const isReady = (): boolean => this.routeRuntime.snapshot(meshName).isReady;
    if (isReady()) {
      this.emit(`bingo-ready kind=mesh-route node=${this.config.nodeId} mesh=${logName}`);
      return;
    }
    const observations = this.routeRuntime.observe(meshName, 64, this.stop.signal);
    if (isReady()) {
      this.emit(`bingo-ready kind=mesh-route node=${this.config.nodeId} mesh=${logName}`);
      return;
    }
    for await (const observed of observations) {
      if (!observed.status.isReady) continue;
      this.emit(`bingo-ready kind=mesh-route node=${this.config.nodeId} mesh=${logName}`);
      return;
    }
  }

  private emit(line: string): void {
    if (this.emitted.has(line)) return;
    this.emitted.add(line);
    console.log(line);
  }
}

export { RoomRouterReadinessHandler };
