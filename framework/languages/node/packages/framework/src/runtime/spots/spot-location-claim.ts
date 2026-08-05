import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import type { RoutingId } from '../../contracts';
import { ZLinkLocationWriteStatus } from '../../contracts/Locations';
import {
  ZLinkSpotKind,
} from '../../contracts';
import { ZLinkConfigurationException } from '../configuration';
import type { ZLinkLocationLifecycle } from '../locations';

interface ZLinkSpotLocationClaimOptions {
  readonly lifecycle?: ZLinkLocationLifecycle;
  readonly nodeRid?: RoutingId;
  readonly nodeRidProvider?: (meshName: string) => RoutingId | undefined;
  readonly nodeGenerationProvider?: (meshName: string) => bigint | undefined;
}

export type ZLinkSpotLocationClaimResult =
  | { readonly claimed: true; readonly meshName: string }
  | { readonly claimed: false; readonly state: 'existing'; readonly meshName: string };

export class ZLinkSpotLocationClaim {
  constructor(private readonly options: ZLinkSpotLocationClaimOptions) {}

  async claimUserSpot(
    meshName: string,
    spotId: RoutingId,
    spotTypeName: string,
    spotGeneration: bigint
  ): Promise<ZLinkSpotLocationClaimResult> {
    if (this.options.lifecycle === undefined) {
      return { claimed: true, meshName: '' };
    }
    requireMeshName(meshName);
    const status = await this.options.lifecycle.claimSpot(
      meshName,
      spotId,
      spotTypeName,
      this.requireNodeRid(meshName),
      ZLinkSpotKind.User,
      requirePositiveGeneration(spotGeneration, 'Spot'),
      requirePositiveGeneration(this.options.nodeGenerationProvider?.(meshName), 'MeshNode')
    );
    if (status === ZLinkLocationWriteStatus.RejectedConflict) {
      return { claimed: false, state: 'existing', meshName };
    }
    if (status !== ZLinkLocationWriteStatus.Stored) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotCreateFailed,
        `Spot '${spotId}' location claim failed with '${status}'.`
      );
    }
    return { claimed: true, meshName };
  }

  get enabled(): boolean {
    return this.options.lifecycle !== undefined;
  }

  async release(meshName: string, spotId: RoutingId): Promise<void> {
    if (this.options.lifecycle === undefined) {
      return;
    }
    await this.options.lifecycle.releaseSpot(meshName, spotId);
  }

  private requireNodeRid(meshName: string): RoutingId {
    const nodeRid = this.options.nodeRidProvider?.(meshName) ?? this.options.nodeRid;
    if (nodeRid === undefined) {
      throw new ZLinkConfigurationException('Location spot claim requires a node routing id.');
    }
    return nodeRid;
  }
}

function requireMeshName(meshName: string): void {
  if (meshName.length === 0) {
    throw new ZLinkConfigurationException('Location spot claim requires a mesh name.');
  }
}

function requirePositiveGeneration(generation: bigint | undefined, owner: string): bigint {
  if (generation === undefined || generation <= 0n) {
    throw new ZLinkConfigurationException(`${owner} lifecycle generation is not available.`);
  }
  return generation;
}
