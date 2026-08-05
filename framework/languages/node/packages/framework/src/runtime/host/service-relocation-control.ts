import {
  decodeMaintenanceRelocationControl,
  encodeMaintenanceRelocationControl,
  type ServiceMaintenanceRelocationControl
} from '../foundation/service-stateful-wire-codec';

export type ZLinkServiceRelocationControlRequest = ServiceMaintenanceRelocationControl;
export type ZLinkServiceRelocationControlResponse = ServiceMaintenanceRelocationControl;

export function encodeServiceRelocationControlRequest(request: ZLinkServiceRelocationControlRequest): Buffer {
  return encodeMaintenanceRelocationControl(request);
}

export function decodeServiceRelocationControlRequest(payload: Uint8Array): ZLinkServiceRelocationControlRequest | undefined {
  const bytes = Buffer.from(payload);
  if (bytes.byteLength < 5 || bytes[0] !== 0x5a || bytes[1] !== 0x4d || bytes[2] !== 1) return undefined;
  return decodeMaintenanceRelocationControl(bytes);
}

export function encodeServiceRelocationControlResponse(response: ZLinkServiceRelocationControlResponse): Buffer {
  return encodeMaintenanceRelocationControl(response);
}

export function decodeServiceRelocationControlResponse(payload: Uint8Array): ZLinkServiceRelocationControlResponse {
  return decodeMaintenanceRelocationControl(payload);
}
