import type { Message } from '../../contracts/Common/Message';
import type { ZLinkFlowOrigin } from '../../contracts';
import {
  decodeChannelHeader,
  ZLINK_CHANNEL_FORMAT_MARKER,
  ZLinkChannelMessageKind,
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

/**
 * Identifiers salvaged from a malformed channel envelope. Spec 27 §7: a
 * dispatch-failure record keeps the correlation and flow values it could read
 * from the invalid frame; it never fabricates fresh identifiers. Each field is
 * present only when it is individually well-formed.
 */
export interface ZLinkMalformedChannelHeaderInfo {
  readonly kind?: ZLinkChannelMessageKind;
  readonly channelName?: string;
  readonly messageName?: string;
  readonly correlationId?: string;
  readonly flowId?: string;
  readonly flowOrigin?: ZLinkFlowOrigin;
}

export function inspectMalformedChannelHeader(
  parts: readonly Message[],
  flowEnabled: boolean
): ZLinkMalformedChannelHeaderInfo {
  if (parts.length === 0) return {};
  let header: Record<string, unknown>;
  try {
    const parsed: unknown = JSON.parse(parts[0].data().toString());
    if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) return {};
    header = parsed as Record<string, unknown>;
  } catch {
    return {};
  }
  if (header.formatMarker !== ZLINK_CHANNEL_FORMAT_MARKER) return {};
  const flowId = typeof header.flowId === 'string' && CHANNEL_FLOW_ID_PATTERN.test(header.flowId)
    ? header.flowId
    : undefined;
  const flowOrigin = salvageFlowOrigin(header.flowOrigin);
  const flowPairValid = flowEnabled && flowId !== undefined && flowOrigin !== undefined;
  return {
    kind: salvageChannelKind(header.kind),
    channelName: salvageString(header.channelName),
    messageName: salvageString(header.messageName),
    correlationId: salvageCorrelationId(header.correlationId),
    flowId: flowPairValid ? flowId : undefined,
    flowOrigin: flowPairValid ? flowOrigin : undefined
  };
}

/**
 * Request-shaped header used to encode the protocol error reply for a
 * malformed request envelope. Carries only identifiers actually read from the
 * invalid frame (spec 27 §7).
 */
export function malformedProtocolErrorRequestHeader(
  channelName: string,
  info: ZLinkMalformedChannelHeaderInfo
): ZLinkChannelEnvelopeHeader {
  return {
    formatMarker: ZLINK_CHANNEL_FORMAT_MARKER,
    kind: ZLinkChannelMessageKind.Request,
    channelName,
    messageName: info.messageName ?? '',
    contentType: 'application/json',
    correlationId: info.correlationId ?? null,
    deadline: null,
    topic: null,
    metadata: {},
    flowId: info.flowId,
    flowOrigin: info.flowOrigin
  };
}

const CHANNEL_FLOW_ID_PATTERN =
  /^[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;

function salvageChannelKind(value: unknown): ZLinkChannelMessageKind | undefined {
  return value === ZLinkChannelMessageKind.Request
    || value === ZLinkChannelMessageKind.Response
    || value === ZLinkChannelMessageKind.Command
    || value === ZLinkChannelMessageKind.Publish
    || value === ZLinkChannelMessageKind.Error
    ? value
    : undefined;
}

function salvageString(value: unknown): string | undefined {
  return typeof value === 'string' && value.length > 0 ? value : undefined;
}

function salvageCorrelationId(value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined;
  const byteLength = Buffer.byteLength(value, 'utf8');
  return byteLength >= 1 && byteLength <= 64 && /^[\x00-\x7f]*$/.test(value)
    ? value
    : undefined;
}

function salvageFlowOrigin(value: unknown): ZLinkFlowOrigin | undefined {
  switch (value) {
    case 1: return 'Inbound';
    case 2: return 'Timer';
    case 3: return 'Application';
    case 4: return 'Lifecycle';
    default: return undefined;
  }
}
