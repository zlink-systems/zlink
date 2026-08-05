const PREFIX_SIZE = 5;
const LIVENESS_FRAME_SIZE = PREFIX_SIZE + 8;

export interface ServiceWireDefinition {
  readonly magic: readonly [number, number];
  readonly major: number;
  readonly commands: Readonly<Record<string, number>> & {
    readonly livenessProbe: number;
    readonly livenessAck: number;
  };
}

export type ServiceWireDecodeErrorCode =
  | 'truncated-field'
  | 'invalid-magic'
  | 'unsupported-major'
  | 'unknown-command'
  | 'forbidden-flag'
  | 'invalid-field'
  | 'trailing-byte';

export class ServiceWireDecodeError extends Error {
  constructor(readonly code: ServiceWireDecodeErrorCode) {
    super(`Service wire decode failed: ${code}.`);
    this.name = 'ServiceWireDecodeError';
  }
}

export interface ServiceWireLivenessRecord {
  readonly command: number;
  readonly probeId: bigint;
}

export interface ServiceWireCodec {
  encodeLivenessRecord(record: ServiceWireLivenessRecord): Buffer;
  decodeLivenessRecord(frame: Uint8Array): ServiceWireLivenessRecord;
}

export function createServiceWireCodec(definition: ServiceWireDefinition): ServiceWireCodec {
  const knownCommands = new Set<number>(Object.values(definition.commands));
  return {
    encodeLivenessRecord(record) {
      if (
        record.command !== definition.commands.livenessProbe &&
        record.command !== definition.commands.livenessAck
      ) {
        throw new RangeError('command must be a liveness command.');
      }
      if (record.probeId <= 0n || record.probeId > 0xffff_ffff_ffff_ffffn) {
        throw new RangeError('probeId must be a non-zero unsigned 64-bit integer.');
      }

      const frame = Buffer.allocUnsafe(LIVENESS_FRAME_SIZE);
      frame[0] = definition.magic[0];
      frame[1] = definition.magic[1];
      frame[2] = definition.major;
      frame[3] = record.command;
      frame[4] = 0;
      frame.writeBigUInt64BE(record.probeId, PREFIX_SIZE);
      return frame;
    },
    decodeLivenessRecord(frame) {
      if (frame.byteLength < PREFIX_SIZE) fail('truncated-field');
      if (frame[0] !== definition.magic[0] || frame[1] !== definition.magic[1]) {
        fail('invalid-magic');
      }
      if (frame[2] !== definition.major) fail('unsupported-major');

      const command = frame[3];
      if (!knownCommands.has(command)) fail('unknown-command');
      if (
        command !== definition.commands.livenessProbe &&
        command !== definition.commands.livenessAck
      ) {
        fail('unknown-command');
      }
      if (frame[4] !== 0) fail('forbidden-flag');
      if (frame.byteLength < LIVENESS_FRAME_SIZE) fail('truncated-field');
      if (frame.byteLength > LIVENESS_FRAME_SIZE) fail('trailing-byte');

      const view = Buffer.from(frame.buffer, frame.byteOffset, frame.byteLength);
      const probeId = view.readBigUInt64BE(PREFIX_SIZE);
      if (probeId === 0n) fail('invalid-field');
      return { command, probeId };
    }
  };
}

function fail(code: ServiceWireDecodeErrorCode): never {
  throw new ServiceWireDecodeError(code);
}
