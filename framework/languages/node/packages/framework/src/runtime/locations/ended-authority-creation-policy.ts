import {
  type ZLinkMeshNodeDescriptor,
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../contracts';
import type { ZLinkPlacementAllocation } from './internal-location-contracts';

/** Location runtime §6.1 reads the policy from the descriptor of this creation attempt. */
export function requireDisabledCreationPolicy(
  allocation: ZLinkPlacementAllocation,
  targetLifecycleGeneration: bigint,
  descriptor: ZLinkMeshNodeDescriptor | undefined
): void {
  const capability =
    descriptor?.lifecycleGeneration === targetLifecycleGeneration
      ? descriptor.objectCapabilities.find(
          (entry) =>
            entry.objectKind === allocation.objectKind && entry.stableType === allocation.stableType
        )
      : undefined;
  if (capability?.policy !== 'disabled') {
    throw new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.Unavailable,
      'Object owner lease is unavailable.'
    );
  }
}
