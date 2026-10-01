import type { ZlinkStreamCloseReason } from '../../Contracts';

export const ZLINK_SESSION_CLOSING = 'session-closing';

const reasons: Readonly<Partial<Record<number, ZlinkStreamCloseReason>>> = {
  1: 'ClientClose',
  2: 'IdleTimeout',
  3: 'HeartbeatTimeout',
  4: 'ServerDrain',
  5: 'ProtocolError',
  6: 'TransportError'
};

export function decodeSessionClosing(payload: Uint8Array): {
  readonly closeReason: ZlinkStreamCloseReason;
  readonly diagnostic?: string;
} {
  if (payload.length < 4 || payload[0] !== 1)
    throw new Error('Unsupported session-closing version.');
  const closeReason = reasons[payload[1]];
  if (closeReason === undefined) throw new Error('Unknown session-closing reason.');
  const length = (payload[2] << 8) | payload[3];
  if (length > 512 || payload.length !== 4 + length)
    throw new Error('Invalid session-closing diagnostic length.');
  const diagnostic =
    length === 0
      ? undefined
      : new TextDecoder('utf-8', { fatal: true }).decode(payload.subarray(4));
  return { closeReason, diagnostic };
}
