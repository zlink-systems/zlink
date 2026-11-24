import type { RoutingId } from '../../contracts/Common/CoreTypes';
import type {
  ZLinkCreationOperationIdentity,
  ZLinkPlacementAllocation
} from './internal-location-contracts';
import { encodeRoutingIdStorageHex } from '../routing-id';

export function opaqueRecordPreimage(record: string, ...segments: readonly string[]): string {
  return [record, ...segments].join('\0');
}

export function creationTerminalPreimage(operation: ZLinkCreationOperationIdentity): string {
  return opaqueRecordPreimage(
    'creation-terminal',
    routingIdHexSegment(operation.sourceNodeRid),
    operation.sourceNodeGeneration.toString(),
    operation.operationId.high.toString(16).padStart(16, '0') +
      operation.operationId.low.toString(16).padStart(16, '0')
  );
}

export function routingIdHexSegment(routingId: RoutingId): string {
  return encodeRoutingIdStorageHex(routingId).toLowerCase();
}

export function canonicalPlacementObjectKind(
  kind: ZLinkPlacementAllocation['objectKind']
): 'actor' | 'userSpot' | 'instanceSpot' {
  switch (kind) {
    case 'actor':
      return 'actor';
    case 'user_spot':
      return 'userSpot';
    case 'instance_spot':
      return 'instanceSpot';
  }
}
