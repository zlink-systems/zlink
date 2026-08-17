import type { Message } from '../../contracts/Common/Message';
import {
  decodeChannelHeader,
  type ZLinkChannelEnvelopeHeader
} from './channel-envelope';

export function tryDecodeChannelHeader(
  parts: readonly Message[],
  flowEnabled = true
): ZLinkChannelEnvelopeHeader | undefined {
  if (parts.length < 2 || parts[0].data().length === 0) {
    return undefined;
  }
  try {
    return decodeChannelHeader(parts, flowEnabled);
  } catch {
    return undefined;
  }
}
