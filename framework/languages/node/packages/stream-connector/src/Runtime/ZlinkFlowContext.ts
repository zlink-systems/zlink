import type { ZlinkFlowOrigin } from '../Contracts';
import { ZlinkStreamErrorCode } from '../Contracts';
import { connectorError } from './ZlinkStreamSupport';

export interface ZlinkFlowContextValue {
  readonly flowId: string;
  readonly flowOrigin: ZlinkFlowOrigin;
}

export interface ZlinkFlowContext {
  currentOrCreate(explicit?: ZlinkFlowContextValue): ZlinkFlowContextValue;
  createInbound(flowId?: string, flowOrigin?: ZlinkFlowOrigin): ZlinkFlowContextValue;
}

interface BrowserCrypto {
  getRandomValues<T extends Uint8Array>(array: T): T;
}

export class BrowserZlinkFlowContext implements ZlinkFlowContext {
  currentOrCreate(explicit?: ZlinkFlowContextValue): ZlinkFlowContextValue {
    return explicit ?? { flowId: this.createUuidV7(), flowOrigin: 'Application' };
  }

  createInbound(flowId?: string, flowOrigin?: ZlinkFlowOrigin): ZlinkFlowContextValue {
    return { flowId: flowId ?? this.createUuidV7(), flowOrigin: flowOrigin ?? 'Inbound' };
  }

  private createUuidV7(): string {
    const crypto = (globalThis as { crypto?: BrowserCrypto }).crypto;
    if (crypto === undefined) {
      throw connectorError(
        ZlinkStreamErrorCode.ConfigurationError,
        'The browser entrypoint requires the platform Web Crypto API.'
      );
    }
    return formatUuidV7(crypto.getRandomValues(new Uint8Array(16)));
  }
}

export function formatUuidV7(bytes: Uint8Array): string {
  const timestamp = BigInt(Date.now());
  for (let index = 5; index >= 0; index -= 1) {
    bytes[index] = Number(timestamp >> BigInt((5 - index) * 8) & 0xffn);
  }
  bytes[6] = 0x70 | (bytes[6] & 0x0f);
  bytes[8] = 0x80 | (bytes[8] & 0x3f);
  const hex = [...bytes].map((byte) => byte.toString(16).padStart(2, '0')).join('');
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}
