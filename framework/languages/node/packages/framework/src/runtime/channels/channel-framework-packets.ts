import { isAdmissionQueuePacket } from '../streams/admission-queue-messages';
import type { ZLinkChannelEnvelopeCodecRegistry } from './channel-envelope';

export function codecsForFrameworkPacket(
  packetName: string | undefined,
  codecs: ZLinkChannelEnvelopeCodecRegistry | undefined
): ZLinkChannelEnvelopeCodecRegistry | undefined {
  return packetName?.startsWith('__zlink.') === true || isAdmissionQueuePacket(packetName)
    ? undefined
    : codecs;
}
