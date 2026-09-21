import type { Message } from '../../contracts/Common/Message';
import {
  FANOUT_LIVENESS_TOPIC,
  requirePublicFanoutTopic
} from '../../contracts/Configuration/FanoutTopic';
import { tryDecodeChannelHeader } from './channel-envelope-inspection';
import type { ZLinkChannelEnvelopeHeader } from './channel-envelope';

export { FANOUT_LIVENESS_TOPIC, requirePublicFanoutTopic };
export const FANOUT_LIVENESS_PAYLOAD = Uint8Array.from([0x5a, 0x46, 0x01, 0x01]);

export type ZLinkFanoutInboundKind = 'application' | 'beacon' | 'protocolError';

export interface ZLinkFanoutInboundClassification {
  readonly kind: ZLinkFanoutInboundKind;
  readonly header?: ZLinkChannelEnvelopeHeader;
}

export function inspectFanoutInbound(
  topic: string,
  parts: readonly { data(): Uint8Array }[]
): ZLinkFanoutInboundClassification {
  if (topic !== FANOUT_LIVENESS_TOPIC) {
    const header = tryDecodeChannelHeader(parts as readonly Message[]);
    return header === undefined ? { kind: 'protocolError' } : { kind: 'application', header };
  }
  if (parts.length !== 1) return { kind: 'protocolError' };
  const payload = parts[0]!.data();
  return payload.length === FANOUT_LIVENESS_PAYLOAD.length &&
    payload.every((value, index) => value === FANOUT_LIVENESS_PAYLOAD[index])
    ? { kind: 'beacon' }
    : { kind: 'protocolError' };
}
