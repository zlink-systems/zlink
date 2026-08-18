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
 * Frames the pre-seal base snapshots of every base/delta-capable participant
 * of one relocation attempt into a single combined blob, keyed by authority
 * key. This is the payload transmitted as payloadStage=base chunks — the
 * relocationPrepare manifest (payloadTotalLength/ChunkCount/ChecksumCrc32c)
 * only describes the final (delta) stage that follows (spec 15 §5).
 */
export function encodeRelocationBaseBundle(
  bases: ReadonlyMap<string, Uint8Array>
): Buffer {
  const parts: Buffer[] = [Buffer.alloc(4)];
  parts[0]!.writeUInt32BE(bases.size, 0);
  for (const [key, data] of bases) {
    const keyBytes = Buffer.from(key, 'utf8');
    if (keyBytes.byteLength > 0xffff) {
      throw new RangeError('Relocation base bundle authority key exceeds the length bound.');
    }
    const header = Buffer.alloc(2 + 4);
    header.writeUInt16BE(keyBytes.byteLength, 0);
    header.writeUInt32BE(data.byteLength, 2);
    parts.push(header, keyBytes, Buffer.from(data));
  }
  return Buffer.concat(parts);
}

export interface ZLinkRelocationBaseBundleFrame {
  readonly blob: Buffer;
  readonly checksumCrc32c: number;
}

/**
 * Encodes the base bundle and computes its baseChecksumCrc32c together — the
 * source's relocationPrepare manifest and the target's
 * {@link ZLinkRelocationBaseStageBuffer.resolve} agree on exactly one
 * checksum definition (the framed blob's CRC-32C), so callers never
 * recompute it themselves.
 */
export function encodeRelocationBaseBundleFramed(
  bases: ReadonlyMap<string, Uint8Array>
): ZLinkRelocationBaseBundleFrame {
  const blob = encodeRelocationBaseBundle(bases);
  return { blob, checksumCrc32c: crc32c(blob) };
}

/**
 * Owns one target's pre-seal base-stage chunk buffer end to end: ordered
 * accumulation under a fixed source+identity fence, then checksum
 * verification and bundle decode once its relocationPrepare manifest
 * resolves it (spec 15 §5). Base-stage chunks travel ahead of that manifest,
 * so this buffer is the sole place that knows how an in-flight base stage is
 * assembled — callers only fence identity per chunk and read back the
 * decoded per-authority snapshot map.
 */
export class ZLinkRelocationBaseStageBuffer {
  private readonly chunks: Buffer[] = [];
  private nextOrdinal = 0;

  constructor(
    private readonly sourceNodeRid: string,
    private readonly fingerprint: string
  ) {}

  /**
   * Appends one in-order chunk under the given identity fence. Returns false
   * (chunk discarded, buffer unchanged) when the fence or ordinal does not
   * continue this buffer — a chunk from a superseded or unrelated attempt.
   */
  accept(ordinal: number, data: Uint8Array, sourceNodeRid: string, fingerprint: string): boolean {
    if (this.sourceNodeRid !== sourceNodeRid
      || this.fingerprint !== fingerprint
      || ordinal !== this.nextOrdinal) {
      return false;
    }
    this.chunks.push(Buffer.from(data));
    this.nextOrdinal += 1;
    return true;
  }

  /**
   * Verifies the assembled blob against the Prepare manifest's identity and
   * checksum, then decodes it into the per-authority base snapshot map.
   */
  resolve(expectedFingerprint: string, baseChecksumCrc32c: number): ReadonlyMap<string, Buffer> {
    if (this.fingerprint !== expectedFingerprint) {
      throw new Error('base chunks do not match the Prepare identity.');
    }
    const blob = Buffer.concat(this.chunks);
    if (crc32c(blob) !== baseChecksumCrc32c) {
      throw new Error('base snapshot failed its checksum.');
    }
    return decodeRelocationBaseBundle(blob);
  }
}

/** Inverse of {@link encodeRelocationBaseBundle}. Throws on a malformed blob. */
export function decodeRelocationBaseBundle(blob: Uint8Array): ReadonlyMap<string, Buffer> {
  const bytes = Buffer.isBuffer(blob) ? blob : Buffer.from(blob);
  if (bytes.byteLength < 4) throw new Error('Relocation base bundle is malformed.');
  const count = bytes.readUInt32BE(0);
  const result = new Map<string, Buffer>();
  let offset = 4;
  for (let index = 0; index < count; index += 1) {
    if (offset + 6 > bytes.byteLength) throw new Error('Relocation base bundle is truncated.');
    const keyLength = bytes.readUInt16BE(offset);
    const dataLength = bytes.readUInt32BE(offset + 2);
    offset += 6;
    if (offset + keyLength + dataLength > bytes.byteLength) {
      throw new Error('Relocation base bundle entry exceeds the blob bounds.');
    }
    const key = bytes.toString('utf8', offset, offset + keyLength);
    offset += keyLength;
    const data = Buffer.from(bytes.subarray(offset, offset + dataLength));
    offset += dataLength;
    result.set(key, data);
  }
  if (offset !== bytes.byteLength) throw new Error('Relocation base bundle has trailing bytes.');
  return result;
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
