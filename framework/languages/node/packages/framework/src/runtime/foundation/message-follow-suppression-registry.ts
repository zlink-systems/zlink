export interface MessageFollowSuppressionFence {
  readonly objectKind: string;
  readonly logicalObjectId: string;
  readonly objectGeneration: string;
  readonly sourceNodeRid: string;
  readonly sourceNodeGeneration: string;
  readonly sourceAuthorityOwnerGeneration: string;
  readonly sourceOwnerLeaseGeneration: string;
  readonly targetNodeRid: string;
  readonly targetNodeGeneration: string;
  readonly targetAuthorityOwnerGeneration: string;
  readonly targetOwnerLeaseGeneration: string;
}

export type MessageFollowSuppressionState =
  | 'idle'
  | 'inFlight'
  | 'sentUntilExpiry';

export interface MessageFollowSuppressionClaim {
  readonly routeKey: string;
  readonly serial: bigint;
}

interface RetainedRouteMarker {
  readonly fence: MessageFollowSuppressionFence;
  state: MessageFollowSuppressionState;
  claimSerial?: bigint;
}

/**
 * Owns one Message Follow send marker per retained route. Route-cache expiry
 * drives marker expiry; this registry never creates an independent timer.
 */
export class MessageFollowSuppressionRegistry {
  private readonly markers = new Map<string, RetainedRouteMarker>();
  private nextClaimSerial = 1n;

  retainRoute(fence: MessageFollowSuppressionFence): void {
    const routeKey = messageFollowSuppressionKey(fence);
    if (this.markers.has(routeKey)) return;
    this.markers.set(routeKey, {
      fence: Object.freeze({ ...fence }),
      state: 'idle'
    });
  }

  replaceRoute(
    previous: MessageFollowSuppressionFence,
    replacement: MessageFollowSuppressionFence
  ): void {
    this.markers.delete(messageFollowSuppressionKey(previous));
    const routeKey = messageFollowSuppressionKey(replacement);
    this.markers.set(routeKey, {
      fence: Object.freeze({ ...replacement }),
      state: 'idle'
    });
  }

  expireRoute(fence: MessageFollowSuppressionFence): boolean {
    return this.markers.delete(messageFollowSuppressionKey(fence));
  }

  begin(fence: MessageFollowSuppressionFence): MessageFollowSuppressionClaim | undefined {
    const routeKey = messageFollowSuppressionKey(fence);
    const marker = this.markers.get(routeKey);
    if (marker === undefined || marker.state !== 'idle') return undefined;
    const serial = this.nextClaimSerial++;
    marker.state = 'inFlight';
    marker.claimSerial = serial;
    return Object.freeze({ routeKey, serial });
  }

  markSent(claim: MessageFollowSuppressionClaim): boolean {
    const marker = this.currentClaim(claim);
    if (marker === undefined) return false;
    marker.state = 'sentUntilExpiry';
    marker.claimSerial = undefined;
    return true;
  }

  abort(claim: MessageFollowSuppressionClaim): boolean {
    const marker = this.currentClaim(claim);
    if (marker === undefined) return false;
    marker.state = 'idle';
    marker.claimSerial = undefined;
    return true;
  }

  state(fence: MessageFollowSuppressionFence): MessageFollowSuppressionState {
    return this.markers.get(messageFollowSuppressionKey(fence))?.state ?? 'idle';
  }

  get retainedRouteCount(): number {
    return this.markers.size;
  }

  private currentClaim(
    claim: MessageFollowSuppressionClaim
  ): RetainedRouteMarker | undefined {
    const marker = this.markers.get(claim.routeKey);
    return marker?.state === 'inFlight' && marker.claimSerial === claim.serial
      ? marker
      : undefined;
  }
}

export function messageFollowSuppressionKey(
  fence: MessageFollowSuppressionFence
): string {
  return [
    fence.objectKind,
    fence.logicalObjectId,
    fence.objectGeneration,
    fence.sourceNodeRid,
    fence.sourceNodeGeneration,
    fence.sourceAuthorityOwnerGeneration,
    fence.sourceOwnerLeaseGeneration,
    fence.targetNodeRid,
    fence.targetNodeGeneration,
    fence.targetAuthorityOwnerGeneration,
    fence.targetOwnerLeaseGeneration
  ].map(lengthPrefixed).join('');
}

function lengthPrefixed(value: string): string {
  return `${value.length}:${value}`;
}
