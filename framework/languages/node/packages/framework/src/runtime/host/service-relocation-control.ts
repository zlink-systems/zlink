import {
  SERVICE_WIRE_MAJOR_OFFSET,
  SERVICE_WIRE_COMMAND_OFFSET,
  SERVICE_WIRE_PREFIX_SIZE
} from '../foundation/service-wire-binary-primitives';
import {
  SERVICE_WIRE_MAGIC,
  SERVICE_WIRE_MAJOR
} from '../foundation/service-wire-constants.generated';
import {
  decodeMaintenanceRelocationControl,
  encodeMaintenanceRelocationControl,
  type ServiceMaintenanceRelocationControl
} from '../foundation/service-stateful-wire-codec';

export type ZLinkServiceRelocationControlRequest = ServiceMaintenanceRelocationControl;
export type ZLinkServiceRelocationControlResponse = ServiceMaintenanceRelocationControl;

export function encodeServiceRelocationControlRequest(
  request: ZLinkServiceRelocationControlRequest
): Buffer {
  return encodeMaintenanceRelocationControl(request);
}

export function decodeServiceRelocationControlRequest(
  payload: Uint8Array
): ZLinkServiceRelocationControlRequest | undefined {
  const bytes = Buffer.isBuffer(payload)
    ? payload
    : Buffer.from(payload.buffer, payload.byteOffset, payload.byteLength);
  if (serviceRelocationControlCommand(bytes) === undefined) return undefined;
  return decodeMaintenanceRelocationControl(bytes);
}

export function serviceRelocationControlCommand(payload: Uint8Array): number | undefined {
  if (
    payload.byteLength < SERVICE_WIRE_PREFIX_SIZE ||
    payload[0] !== SERVICE_WIRE_MAGIC[0] ||
    payload[1] !== SERVICE_WIRE_MAGIC[1] ||
    payload[SERVICE_WIRE_MAJOR_OFFSET] !== SERVICE_WIRE_MAJOR
  )
    return undefined;
  return payload[SERVICE_WIRE_COMMAND_OFFSET];
}

export function encodeServiceRelocationControlResponse(
  response: ZLinkServiceRelocationControlResponse
): Buffer {
  return encodeMaintenanceRelocationControl(response);
}

export function decodeServiceRelocationControlResponse(
  payload: Uint8Array
): ZLinkServiceRelocationControlResponse {
  return decodeMaintenanceRelocationControl(payload);
}
