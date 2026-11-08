import { randomBytes } from 'node:crypto';
import { MAX_TIME_MS } from './encoding';

export const monotonicMs = () => process.hrtime.bigint() / 1_000_000n;
export interface Snapshot {
  cluster: string;
  revision: string;
  clock: bigint;
  started: bigint;
  last: Buffer;
}

/** Binary fields keep the longest UTF-8 prefix/key cursor below the SPI bound. */
export class ScanCursor {
  private readonly instance = randomBytes(16);
  constructor(private readonly namespace: string) {}

  encode(snapshot: Snapshot, prefix: string): string {
    const fields = [Buffer.from(this.namespace), Buffer.from(prefix), snapshot.last];
    const header = Buffer.alloc(54);
    header[0] = 1;
    this.instance.copy(header, 1);
    header.writeBigUInt64BE(BigInt(snapshot.cluster), 17);
    header.writeBigUInt64BE(BigInt(snapshot.revision), 25);
    header.writeBigUInt64BE(snapshot.clock, 33);
    header.writeBigUInt64BE(snapshot.started, 41);
    header[49] = fields[0].length;
    header.writeUInt16BE(fields[1].length, 50);
    header.writeUInt16BE(fields[2].length, 52);
    return Buffer.concat([header, ...fields]).toString('base64url');
  }

  decode(value: string, prefix: string): Snapshot | undefined {
    if (!/^[A-Za-z0-9_-]+$/.test(value)) return undefined;
    const bytes = Buffer.from(value, 'base64url');
    if (
      bytes.length < 54 ||
      bytes.toString('base64url') !== value ||
      bytes[0] !== 1 ||
      !bytes.subarray(1, 17).equals(this.instance)
    )
      return undefined;
    const namespaceEnd = 54 + bytes[49];
    const prefixEnd = namespaceEnd + bytes.readUInt16BE(50);
    if (
      bytes.length !== prefixEnd + bytes.readUInt16BE(52) ||
      !bytes.subarray(54, namespaceEnd).equals(Buffer.from(this.namespace)) ||
      !bytes.subarray(namespaceEnd, prefixEnd).equals(Buffer.from(prefix))
    )
      return undefined;
    const snapshot = {
      cluster: bytes.readBigUInt64BE(17).toString(),
      revision: bytes.readBigUInt64BE(25).toString(),
      clock: bytes.readBigUInt64BE(33),
      started: bytes.readBigUInt64BE(41),
      last: bytes.subarray(prefixEnd)
    };
    if (
      snapshot.clock > MAX_TIME_MS ||
      snapshot.revision === '0' ||
      BigInt(snapshot.revision) > 0x7fffffffffffffffn ||
      snapshot.last.length > 1024 ||
      !snapshot.last.subarray(0, Buffer.byteLength(prefix)).equals(Buffer.from(prefix)) ||
      !this.valid(snapshot)
    )
      return undefined;
    return snapshot;
  }

  valid(snapshot: Snapshot): boolean {
    const elapsed = monotonicMs() - snapshot.started;
    return elapsed >= 0n && elapsed < 300_000n;
  }
}
