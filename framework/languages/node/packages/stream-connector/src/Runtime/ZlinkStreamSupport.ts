import {
  Disposable,
  ZlinkStreamError,
  ZlinkStreamErrorCode,
  ZlinkStreamException
} from '../Contracts';

export function connectorError(code: ZlinkStreamErrorCode, message: string, cause?: unknown): ZlinkStreamException {
  return new ZlinkStreamException({ code, message, cause });
}

export function toStreamError(cause: unknown, code: ZlinkStreamErrorCode, message: string): ZlinkStreamError {
  if (cause instanceof ZlinkStreamException) {
    return cause.error;
  }
  return { code, message, cause };
}

export function unwrapStreamError(error: unknown): ZlinkStreamError {
  if (error instanceof ZlinkStreamException) {
    return error.error;
  }
  return { code: ZlinkStreamErrorCode.RemoteError, message: error instanceof Error ? error.message : String(error), cause: error };
}

export function subscription(dispose: () => void): Disposable {
  return { dispose };
}

export function throwIfAborted(signal: AbortSignal | undefined): void {
  if (signal?.aborted === true) {
    throw connectorError(ZlinkStreamErrorCode.Disconnected, 'Operation canceled.');
  }
}

export function delay(delayMs: number, signal: AbortSignal | undefined): Promise<void> {
  throwIfAborted(signal);
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      signal?.removeEventListener('abort', onAbort);
      resolve();
    }, delayMs);
    const onAbort = () => {
      clearTimeout(timeout);
      reject(connectorError(ZlinkStreamErrorCode.Disconnected, 'Operation canceled.'));
    };
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

export function readLength(source: Uint8Array, offset: number, bytes: 1 | 2, label: string): number {
  if (source.length - offset < bytes) {
    throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, `Metadata ${label} length is missing.`);
  }
  return bytes === 1 ? source[offset] : readUInt16BE(source, offset);
}

export function writeUInt16BE(destination: Uint8Array, offset: number, value: number): void {
  destination[offset] = (value >>> 8) & 0xff;
  destination[offset + 1] = value & 0xff;
}

export function writeUInt32BE(destination: Uint8Array, offset: number, value: number): void {
  destination[offset] = (value >>> 24) & 0xff;
  destination[offset + 1] = (value >>> 16) & 0xff;
  destination[offset + 2] = (value >>> 8) & 0xff;
  destination[offset + 3] = value & 0xff;
}

export function readUInt16BE(source: Uint8Array, offset: number): number {
  return (source[offset] << 8) | source[offset + 1];
}

export function readUInt32BE(source: Uint8Array, offset: number): number {
  return (
    source[offset] * 0x1000000
    + ((source[offset + 1] << 16) | (source[offset + 2] << 8) | source[offset + 3])
  );
}

export function writeBigUInt64BE(destination: Uint8Array, offset: number, value: bigint): void {
  for (let i = 7; i >= 0; i--) {
    destination[offset + i] = Number(value & 0xffn);
    value >>= 8n;
  }
}

export function readBigUInt64BE(source: Uint8Array, offset: number): bigint {
  let value = 0n;
  for (let i = 0; i < 8; i++) {
    value = (value << 8n) | BigInt(source[offset + i]);
  }
  return value;
}

export function utf8Encode(value: string): Uint8Array {
  return new TextEncoder().encode(value);
}

export function utf8Decode(value: Uint8Array): string {
  return new TextDecoder().decode(value);
}
