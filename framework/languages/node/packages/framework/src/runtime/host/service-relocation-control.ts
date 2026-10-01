import {
  SERVICE_WIRE_MAJOR_OFFSET,
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
  if (
    bytes.byteLength < SERVICE_WIRE_PREFIX_SIZE ||
    bytes[0] !== SERVICE_WIRE_MAGIC[0] ||
    bytes[1] !== SERVICE_WIRE_MAGIC[1] ||
    bytes[SERVICE_WIRE_MAJOR_OFFSET] !== SERVICE_WIRE_MAJOR
  )
    return undefined;
  return decodeMaintenanceRelocationControl(bytes);
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
