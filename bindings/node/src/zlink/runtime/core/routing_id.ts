// SPDX-License-Identifier: MPL-2.0

import { RoutingId } from '../../contracts/core';
import { ConfigError, ConfigResult } from '../../contracts/errors/errors';
import { withRuntimeErrorMessage } from '../errors/error_state';

const ROUTING_ID_MAX_LENGTH = 255;

interface RoutingIdState {
  _bytes: Buffer;
}

function requireRoutingId(routingId: RoutingId, name = 'routingId'): Buffer {
  if (!(routingId instanceof RoutingId)) {
    throw new TypeError(`${name} must be a RoutingId`);
  }
  return (routingId as unknown as RoutingIdState)._bytes;
}

export function normalizeRoutingId(routingId: RoutingId, name = 'routingId'): Buffer {
  return requireRoutingId(routingId, name);
}

export function routingIdFromOwnedBuffer(bytes: Buffer): RoutingId {
  if (!Buffer.isBuffer(bytes)) {
    throw new TypeError('bytes must be a Buffer');
  }
  if (bytes.length === 0 || bytes.length > ROUTING_ID_MAX_LENGTH) {
    throw withRuntimeErrorMessage(
      new ConfigError(ConfigResult.InvalidArgument, 0),
      `bytes must be 1..${ROUTING_ID_MAX_LENGTH} bytes`
    );
  }
  const routingId = Object.create(RoutingId.prototype) as RoutingId;
  (routingId as unknown as RoutingIdState)._bytes = bytes;
  Object.freeze(routingId);
  return routingId;
}
