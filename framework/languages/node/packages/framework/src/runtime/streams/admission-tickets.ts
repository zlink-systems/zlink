import {
  createHmac,
  createSecretKey,
  randomBytes,
  timingSafeEqual,
  type KeyObject
} from 'node:crypto';
import { decodeAdmissionQueueKey } from '../../contracts/Configuration/AdmissionQueueKey';

import type { AdmissionConnection } from './admission-queue-messages';

const version = 1;
const bodyLength = 17;
const ticketLength = bodyLength + 32;
const clockSkewMs = 2_000;
const domain = Buffer.from('zlink-admission-ticket-v1\0', 'ascii');

export class AdmissionTickets {
  readonly #currentKey: KeyObject;
  readonly #previousKey: KeyObject | undefined;

  constructor(currentKey: string, previousKey?: string) {
    this.#currentKey = createSecretKey(decodeAdmissionQueueKey(currentKey, 'admissionQueueKey'));
    this.#previousKey =
      previousKey === undefined
        ? undefined
        : createSecretKey(decodeAdmissionQueueKey(previousKey, 'admissionQueuePreviousKey'));
  }

  static newEpoch(): bigint {
    let epoch: bigint;
    do {
      epoch = randomBytes(8).readBigUInt64BE();
    } while (epoch === 0n);
    return epoch;
  }

  issue(connection: AdmissionConnection, epoch: bigint, expiresAtUnixMs: number): string {
    const body = Buffer.alloc(bodyLength);
    body[0] = version;
    body.writeBigUInt64BE(BigInt(expiresAtUnixMs), 1);
    body.writeBigUInt64BE(epoch, 9);
    return Buffer.concat([body, sign(this.#currentKey, connection, body)]).toString('base64url');
  }

  verify(
    ticket: string | undefined,
    connection: AdmissionConnection,
    epoch: bigint | undefined,
    nowUnixMs: number
  ): boolean {
    if (ticket?.length !== 66 || epoch === undefined || epoch === 0n) return false;
    const bytes = Buffer.from(ticket, 'base64url');
    if (
      bytes.length !== ticketLength ||
      bytes[0] !== version ||
      bytes.toString('base64url') !== ticket
    ) {
      return false;
    }
    if (
      bytes.readBigUInt64BE(9) !== epoch ||
      BigInt(nowUnixMs) > bytes.readBigUInt64BE(1) + BigInt(clockSkewMs)
    ) {
      return false;
    }
    const body = bytes.subarray(0, bodyLength);
    const expected = bytes.subarray(bodyLength);
    return (
      timingSafeEqual(expected, sign(this.#currentKey, connection, body)) ||
      (this.#previousKey !== undefined &&
        timingSafeEqual(expected, sign(this.#previousKey, connection, body)))
    );
  }
}

function sign(key: KeyObject, connection: AdmissionConnection, body: Buffer): Buffer {
  const generation = Buffer.alloc(8);
  generation.writeBigUInt64BE(BigInt(connection.nodeLifecycleGeneration));
  const mac = createHmac('sha256', key).update(domain);
  updateIdentityPart(mac, Buffer.from(connection.nodeRid, 'base64url'));
  mac.update(generation);
  updateIdentityPart(mac, Buffer.from(connection.streamName, 'utf8'));
  updateIdentityPart(mac, Buffer.from(connection.sessionRid, 'base64url'));
  return mac.update(body.subarray(1)).digest();
}

function updateIdentityPart(mac: ReturnType<typeof createHmac>, part: Buffer): void {
  if (part.byteLength > 255)
    throw new RangeError('Admission connection identity part is too long.');
  mac.update(Uint8Array.of(part.byteLength));
  mac.update(part);
}
