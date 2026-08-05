// SPDX-License-Identifier: MPL-2.0

import { RoutingId } from '../../contracts/core';

export function wrapRoutingId(routingId: Buffer | Uint8Array | null | undefined): RoutingId | null {
  if (!routingId || routingId.length === 0) {
    return null;
  }
  return RoutingId.from(routingId);
}
