import type { RoutingId } from '../../contracts/Common';
import {
  type ZLinkActorLocationKey,
  type ZLinkPeerLocationKey,
  type ZLinkRouteLocationKey,
  type ZLinkSpotLocationKey
} from './internal-location-contracts';
import { zlinkLocationAutoConnectTypeName, zlinkLocationRoleName } from './canonical-codec';
import { encodeRoutingIdStorageHex } from '../routing-id';

// Framework bookkeeping codec for in-memory row maps and generation guards.
// Backend extensions keep their own storage codec so transport key layout
// changes do not leak into runtime models.
export const ZLinkLocationKeyCodec = Object.freeze({
  encodePeerKey(key: ZLinkPeerLocationKey): string {
    const identity = key.nodeRid === undefined ? key.endpoint ?? '' : encodeRoutingIdHex(key.nodeRid);
    return encodeSegments(
      zlinkLocationAutoConnectTypeName(key.autoConnectType),
      key.meshName,
      zlinkLocationRoleName(key.role),
      identity
    );
  },

  encodeSpotKey(key: ZLinkSpotLocationKey): string {
    return encodeSegments(key.meshName, requireSpotId(key.spotId));
  },

  encodeActorKey(key: ZLinkActorLocationKey): string {
    return encodeSegments(key.meshName, key.actorId);
  },

  encodeRouteKey(key: ZLinkRouteLocationKey): string {
    return encodeSegments(String(key.routeKind), key.routeKey);
  },

  encodeRoutingIdHex,
  normalizeActorType
});

export function encodeZLinkLocationKeySegments(...segments: readonly string[]): string {
  return encodeSegments(...segments);
}

function normalizeActorType(actorType: string | undefined): string {
  return actorType ?? '';
}

function encodeSegments(...segments: readonly string[]): string {
  return segments.map(
    (segment) => `${Buffer.byteLength(segment, 'utf8')}:${segment}`
  ).join('');
}

function requireSpotId(value: string): string {
  const bytes = Buffer.byteLength(value, 'utf8');
  if (bytes < 1 || bytes > 255) {
    throw new TypeError('SpotId must contain 1..255 UTF-8 bytes.');
  }
  return value;
}

function encodeRoutingIdHex(routingId: RoutingId): string {
  return encodeRoutingIdStorageHex(routingId);
}
