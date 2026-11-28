import { ZlinkStreamCodec, ZlinkStreamErrorCode } from '../../Contracts';
import { connectorError, utf8Decode } from '../ZlinkStreamSupport';

export const ADMISSION_ENQUEUE = '$zlink.admission.enqueue';
export const ADMISSION_POSITION = '$zlink.admission.position';
export const ADMISSION_TICKET = '$zlink.admission.ticket';
export const ADMISSION_QUEUE_REQUIRED = '$zlink.admission.queueRequired';
export const ADMISSION_RETRY_AFTER = '$zlink.admission.retryAfterMs';

// Matches the Framework server's admissionTicketEncodedSize.
const admissionTicketEncodedSize = 66;
const admissionTicketPattern = new RegExp(
  `^[A-Za-z0-9_-]{${admissionTicketEncodedSize - 1}}[AQgw]$`
);

export interface AdmissionPosition {
  readonly position: number;
  readonly estimatedWaitMs: number;
}

interface AdmissionTicket {
  readonly ticket: string;
  readonly expiresInMs: number;
}

function integer(
  value: unknown,
  minimum: number,
  maximum = Number.MAX_SAFE_INTEGER
): value is number {
  return (
    typeof value === 'number' && Number.isSafeInteger(value) && value >= minimum && value <= maximum
  );
}

export function decodeAdmissionQueue(
  codec: ZlinkStreamCodec,
  payload: () => Uint8Array,
  name?: string
): AdmissionPosition | AdmissionTicket {
  try {
    if (codec === ZlinkStreamCodec.Json) {
      const value = JSON.parse(utf8Decode(payload())) as {
        position?: unknown;
        estimatedWaitMs?: unknown;
        ticket?: unknown;
        expiresInMs?: unknown;
      } | null;
      if (value !== null && typeof value === 'object') {
        if (
          name !== ADMISSION_POSITION &&
          value.position === undefined &&
          value.estimatedWaitMs === undefined &&
          typeof value.ticket === 'string' &&
          admissionTicketPattern.test(value.ticket) &&
          integer(value.expiresInMs, 1)
        ) {
          return { ticket: value.ticket, expiresInMs: value.expiresInMs };
        }
        if (
          name !== ADMISSION_TICKET &&
          value.ticket === undefined &&
          value.expiresInMs === undefined &&
          integer(value.position, 1) &&
          integer(value.estimatedWaitMs, 0, 2_147_483_647)
        ) {
          return { position: value.position, estimatedWaitMs: value.estimatedWaitMs };
        }
      }
    }
  } catch (cause) {
    throw connectorError(
      ZlinkStreamErrorCode.FrameDecodeFailed,
      'Invalid admission queue payload.',
      cause
    );
  }
  throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, 'Invalid admission queue payload.');
}
