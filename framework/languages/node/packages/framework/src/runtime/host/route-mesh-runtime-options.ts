import type {
  ZLinkMeshChannelRuntimeOptions,
  ZLinkMeshPlacementRuntimeOptions,
  ZLinkRouteMeshRuntimeOptions
} from '../../contracts';
import type { ZLinkSpotNodeRuntimeManager } from '../spots';

export class DefaultZLinkRouteMeshRuntimeOptions implements ZLinkRouteMeshRuntimeOptions {
  constructor(
    private readonly manager: () => ZLinkSpotNodeRuntimeManager | undefined
  ) {}

  mesh(meshName: string): ZLinkMeshPlacementRuntimeOptions {
    const manager = this.requireManager();
    manager.placementWeight(meshName);
    return {
      get placementWeight() {
        return manager.placementWeight(meshName);
      },
      set placementWeight(weight: number) {
        manager.setRuntimePlacementWeight(meshName, weight);
      }
    };
  }

  channel(channelName: string): ZLinkMeshChannelRuntimeOptions {
    const manager = this.requireManager();
    manager.channelWeight(channelName);
    return {
      get weight() {
        return manager.channelWeight(channelName);
      },
      set weight(weight: number) {
        manager.setRuntimeChannelWeight(channelName, weight);
      }
    };
  }

  private requireManager(): ZLinkSpotNodeRuntimeManager {
    const manager = this.manager();
    if (manager === undefined) {
      throw new Error('RouteMesh runtime options require a started Framework runtime.');
    }
    return manager;
  }
}
