import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendMessageLike as MessageLike } from '../backend/runtime-values';
import { ZLinkConfigurationException } from '../configuration';
import { ZLinkChannelMessageKind } from './channel-envelope';

const SPOT_DIRECT_ENVELOPE = 'zlink.framework.spot-direct.v1';

export function encodeSpotDirectEnvelope(
  kind: ZLinkChannelMessageKind.Request | ZLinkChannelMessageKind.Command,
  channelName: string,
  packetName: string | undefined,
  payload: unknown,
  metadata?: ReadonlyMap<string, string>
): MessageLike {
  return Buffer.from(JSON.stringify({
    marker: SPOT_DIRECT_ENVELOPE,
    kind,
    channelName,
    packetName,
    payload,
    metadata: Object.fromEntries(metadata ?? [])
  }));
}

export function decodeSpotDirectReply<TReply>(parts: readonly Message[]): TReply {
  if (parts.length < 1) {
    throw new ZLinkConfigurationException('Spot direct reply is missing.');
  }
  const reply = JSON.parse(parts[0].data().toString()) as {
    readonly marker?: unknown;
    readonly ok?: unknown;
    readonly response?: unknown;
    readonly error?: unknown;
  };
  if (reply.marker !== SPOT_DIRECT_ENVELOPE) {
    throw new ZLinkConfigurationException('Spot direct reply marker is invalid.');
  }
  if (reply.ok !== true) {
    throw new ZLinkConfigurationException(
      typeof reply.error === 'string' ? reply.error : 'Spot direct request failed.'
    );
  }
  return reply.response as TReply;
}
