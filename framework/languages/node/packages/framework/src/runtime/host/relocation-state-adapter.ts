import { crc32c } from '../foundation/service-relocation-runtime';
import {
  zlinkRelocationAdapterHasBaseDelta
} from '../../contracts/Configuration/ObjectRoles';

export interface ZLinkRelocationStateAdapterLike<TInstance> {
  capture(instance: TInstance, signal: AbortSignal): Promise<Uint8Array>;
  restore(instance: TInstance, payload: Uint8Array, signal: AbortSignal): Promise<void>;
  captureBase?(instance: TInstance, signal: AbortSignal): Promise<Uint8Array>;
  captureDelta?(instance: TInstance, signal: AbortSignal): Promise<Uint8Array>;
  restoreBase?(instance: TInstance, payload: Uint8Array, signal: AbortSignal): Promise<void>;
  applyDelta?(instance: TInstance, payload: Uint8Array, signal: AbortSignal): Promise<void>;
}

export { zlinkRelocationAdapterHasBaseDelta as relocationAdapterHasBaseDelta };

const BASE_DELTA_MAGIC = Buffer.from([0x5a, 0x4c, 0x42, 0x44]); // 'ZLBD'
const BASE_DELTA_VERSION = 1;

/**
 * Frames a base snapshot and its delta into one opaque relocation payload.
 * The delta references its base by CRC-32C so the target never applies a
 * delta to a different base snapshot (spec 15 §5).
 */
export function encodeRelocationBaseDeltaState(
  base: Uint8Array,
  delta: Uint8Array
): Buffer {
  const header = Buffer.alloc(4 + 1 + 4 + 8 + 8);
  BASE_DELTA_MAGIC.copy(header, 0);
  header.writeUInt8(BASE_DELTA_VERSION, 4);
  header.writeUInt32BE(crc32c(base), 5);
  header.writeBigUInt64BE(BigInt(base.byteLength), 9);
  header.writeBigUInt64BE(BigInt(delta.byteLength), 17);
  return Buffer.concat([header, Buffer.from(base), Buffer.from(delta)]);
}

export interface ZLinkRelocationBaseDeltaState {
  readonly base: Buffer;
  readonly delta: Buffer;
  readonly baseChecksumCrc32c: number;
}

/** Returns undefined for a payload that is not a base/delta frame. */
export function decodeRelocationBaseDeltaState(
  payload: Uint8Array
): ZLinkRelocationBaseDeltaState | undefined {
  const bytes = Buffer.isBuffer(payload) ? payload : Buffer.from(payload);
  if (bytes.byteLength < 25 || !bytes.subarray(0, 4).equals(BASE_DELTA_MAGIC)
    || bytes.readUInt8(4) !== BASE_DELTA_VERSION) {
    return undefined;
  }
  const baseChecksumCrc32c = bytes.readUInt32BE(5);
  const baseLength = Number(bytes.readBigUInt64BE(9));
  const deltaLength = Number(bytes.readBigUInt64BE(17));
  if (!Number.isSafeInteger(baseLength) || !Number.isSafeInteger(deltaLength)
    || 25 + baseLength + deltaLength !== bytes.byteLength) {
    throw new Error('Relocation base/delta frame is malformed.');
  }
  const base = bytes.subarray(25, 25 + baseLength);
  if (crc32c(base) !== baseChecksumCrc32c) {
    throw new Error('Relocation base snapshot failed its checksum reference.');
  }
  return {
    base,
    delta: bytes.subarray(25 + baseLength),
    baseChecksumCrc32c
  };
}

/**
 * Captures the application state through the adapter. When the adapter
 * registers base/delta, `preSealBase` (captured before the admission seal)
 * pairs with the post-seal delta so the seal window only pays for
 * captureDelta.
 */
export async function captureRelocationAdapterState<TInstance>(
  adapter: ZLinkRelocationStateAdapterLike<TInstance>,
  instance: TInstance,
  preSealBase: Uint8Array | undefined,
  signal: AbortSignal
): Promise<Buffer> {
  if (!zlinkRelocationAdapterHasBaseDelta(adapter)) {
    return Buffer.from(await adapter.capture(instance, signal));
  }
  const base = preSealBase ?? await adapter.captureBase!(instance, signal);
  const delta = await adapter.captureDelta!(instance, signal);
  return encodeRelocationBaseDeltaState(base, delta);
}

/**
 * Restores the application state through the adapter. A base/delta frame is
 * restored with restoreBase then applyDelta; when applyDelta fails and a
 * fresh-instance factory is provided, the partially applied instance is
 * discarded and the restore repeats from restoreBase on the new instance —
 * a partially applied instance is never reused (spec 15 §5).
 */
export async function restoreRelocationAdapterState<TInstance>(
  adapter: ZLinkRelocationStateAdapterLike<TInstance>,
  instance: TInstance,
  payload: Uint8Array,
  signal: AbortSignal,
  recreateInstance?: () => Promise<TInstance>
): Promise<TInstance> {
  const frame = zlinkRelocationAdapterHasBaseDelta(adapter)
    ? decodeRelocationBaseDeltaState(payload)
    : undefined;
  if (frame === undefined) {
    await adapter.restore(instance, payload, signal);
    return instance;
  }
  await adapter.restoreBase!(instance, frame.base, signal);
  try {
    await adapter.applyDelta!(instance, frame.delta, signal);
    return instance;
  } catch (error) {
    if (recreateInstance === undefined) throw error;
    const fresh = await recreateInstance();
    await adapter.restoreBase!(fresh, frame.base, signal);
    await adapter.applyDelta!(fresh, frame.delta, signal);
    return fresh;
  }
}
