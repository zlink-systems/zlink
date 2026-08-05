export const ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET = '__zlink.actor.bound_session.send';
export const ZLINK_REMOTE_BOUND_SESSION_RESPONSE_PACKET = '__zlink.actor.bound_session.response';
export const ZLINK_REMOTE_BOUND_SESSION_ERROR_PACKET = '__zlink.actor.bound_session.error';
export const ZLINK_REMOTE_BOUND_SESSION_OWNERSHIP_PACKET = '__zlink.actor.bound_session.ownership';
export const ZLINK_REMOTE_BOUND_SESSION_SEAL_PACKET = '__zlink.actor.bound_session.relocation_seal';
export const ZLINK_REMOTE_BOUND_SESSION_ABORT_SEAL_PACKET = '__zlink.actor.bound_session.relocation_abort';

export interface ZLinkRemoteBoundSessionSealPayload {
  readonly actorId: string;
  readonly actorGeneration: string;
  readonly actorOwnershipGeneration: string;
  readonly bindingGeneration: string;
  readonly ownerLeaseGeneration: string;
  readonly sealId: string;
}

export interface ZLinkRemoteBoundSessionSealAck {
  readonly actorId: string;
  readonly sealId: string;
  readonly acceptedHighWater: string;
}

export function encodeRemoteBoundSessionSealPayload(
  input: ZLinkRemoteBoundSessionSealPayload,
  abort = false
): Record<string, unknown> {
  return {
    packetName: abort
      ? ZLINK_REMOTE_BOUND_SESSION_ABORT_SEAL_PACKET
      : ZLINK_REMOTE_BOUND_SESSION_SEAL_PACKET,
    ...input
  };
}

export function decodeRemoteBoundSessionSealPayload(
  payload: unknown
): ZLinkRemoteBoundSessionSealPayload & { readonly abort: boolean } {
  const packetName = (payload as { packetName?: unknown } | null)?.packetName;
  if (
    typeof payload !== 'object' || payload === null ||
    (packetName !== ZLINK_REMOTE_BOUND_SESSION_SEAL_PACKET &&
      packetName !== ZLINK_REMOTE_BOUND_SESSION_ABORT_SEAL_PACKET) ||
    typeof (payload as { actorId?: unknown }).actorId !== 'string' ||
    typeof (payload as { actorGeneration?: unknown }).actorGeneration !== 'string' ||
    typeof (payload as { actorOwnershipGeneration?: unknown }).actorOwnershipGeneration !== 'string' ||
    typeof (payload as { bindingGeneration?: unknown }).bindingGeneration !== 'string' ||
    typeof (payload as { ownerLeaseGeneration?: unknown }).ownerLeaseGeneration !== 'string' ||
    typeof (payload as { sealId?: unknown }).sealId !== 'string'
  ) {
    throw new Error('Remote bound session relocation seal payload is invalid.');
  }
  return {
    actorId: (payload as { actorId: string }).actorId,
    actorGeneration: (payload as { actorGeneration: string }).actorGeneration,
    actorOwnershipGeneration: (payload as { actorOwnershipGeneration: string }).actorOwnershipGeneration,
    bindingGeneration: (payload as { bindingGeneration: string }).bindingGeneration,
    ownerLeaseGeneration: (payload as { ownerLeaseGeneration: string }).ownerLeaseGeneration,
    sealId: (payload as { sealId: string }).sealId,
    abort: packetName === ZLINK_REMOTE_BOUND_SESSION_ABORT_SEAL_PACKET
  };
}

export function decodeRemoteBoundSessionSealAck(payload: unknown): ZLinkRemoteBoundSessionSealAck {
  if (
    typeof payload !== 'object' || payload === null ||
    typeof (payload as { actorId?: unknown }).actorId !== 'string' ||
    typeof (payload as { sealId?: unknown }).sealId !== 'string' ||
    typeof (payload as { acceptedHighWater?: unknown }).acceptedHighWater !== 'string'
  ) {
    throw new Error('Remote bound session relocation seal ACK is invalid.');
  }
  return payload as ZLinkRemoteBoundSessionSealAck;
}

export interface ZLinkRemoteBoundSessionOwnershipPayload {
  readonly actorId: string;
  readonly meshName: string;
  readonly actorNodeRid: string;
  readonly actorNodeRidHex?: string;
  readonly actorGeneration: string;
  readonly previousActorOwnershipGeneration: string;
  readonly actorOwnershipGeneration: string;
  readonly bindingGeneration: string;
  readonly previousOwnerLeaseGeneration: string;
  readonly targetOwnerLeaseGeneration: string;
  readonly acceptedHighWater: string;
  readonly sealId: string;
  readonly acceptedJournalReference: string;
  readonly acceptedJournalChecksumCrc32c: number;
  readonly actorPacketTarget?: unknown;
}

export interface ZLinkRemoteBoundSessionOwnershipAck {
  readonly actorId: string;
  readonly actorGeneration: string;
  readonly actorOwnershipGeneration: string;
  readonly bindingGeneration: string;
  readonly targetOwnerLeaseGeneration: string;
  readonly acceptedHighWater: string;
  readonly sealId: string;
}

export function encodeRemoteBoundSessionOwnershipPayload(
  input: ZLinkRemoteBoundSessionOwnershipPayload
): Record<string, unknown> {
  return { packetName: ZLINK_REMOTE_BOUND_SESSION_OWNERSHIP_PACKET, ...input };
}

export function decodeRemoteBoundSessionOwnershipPayload(
  payload: unknown
): ZLinkRemoteBoundSessionOwnershipPayload {
  if (
    typeof payload !== 'object' ||
    payload === null ||
    ((payload as { packetName?: unknown }).packetName !== undefined &&
      (payload as { packetName?: unknown }).packetName !== ZLINK_REMOTE_BOUND_SESSION_OWNERSHIP_PACKET) ||
    typeof (payload as { actorId?: unknown }).actorId !== 'string' ||
    ((payload as { meshName?: unknown }).meshName !== undefined &&
      typeof (payload as { meshName?: unknown }).meshName !== 'string') ||
    typeof (payload as { actorNodeRid?: unknown }).actorNodeRid !== 'string' ||
    typeof (payload as { actorGeneration?: unknown }).actorGeneration !== 'string' ||
    typeof (payload as { previousActorOwnershipGeneration?: unknown }).previousActorOwnershipGeneration !== 'string' ||
    typeof (payload as { actorOwnershipGeneration?: unknown }).actorOwnershipGeneration !== 'string' ||
    typeof (payload as { bindingGeneration?: unknown }).bindingGeneration !== 'string' ||
    typeof (payload as { previousOwnerLeaseGeneration?: unknown }).previousOwnerLeaseGeneration !== 'string' ||
    typeof (payload as { targetOwnerLeaseGeneration?: unknown }).targetOwnerLeaseGeneration !== 'string' ||
    typeof (payload as { acceptedHighWater?: unknown }).acceptedHighWater !== 'string' ||
    typeof (payload as { sealId?: unknown }).sealId !== 'string' ||
    typeof (payload as { acceptedJournalReference?: unknown }).acceptedJournalReference !== 'string' ||
    typeof (payload as { acceptedJournalChecksumCrc32c?: unknown }).acceptedJournalChecksumCrc32c !== 'number'
  ) {
    throw new Error('Remote bound session ownership payload is invalid.');
  }
  return {
    actorId: (payload as { actorId: string }).actorId,
    meshName: (payload as { meshName?: string }).meshName ?? '',
    actorNodeRid: (payload as { actorNodeRid: string }).actorNodeRid,
    actorNodeRidHex: optionalString(payload, 'actorNodeRidHex'),
    actorGeneration: (payload as { actorGeneration: string }).actorGeneration,
    previousActorOwnershipGeneration:
      (payload as { previousActorOwnershipGeneration: string }).previousActorOwnershipGeneration,
    actorOwnershipGeneration: (payload as { actorOwnershipGeneration: string }).actorOwnershipGeneration,
    bindingGeneration: (payload as { bindingGeneration: string }).bindingGeneration,
    previousOwnerLeaseGeneration:
      (payload as { previousOwnerLeaseGeneration: string }).previousOwnerLeaseGeneration,
    targetOwnerLeaseGeneration:
      (payload as { targetOwnerLeaseGeneration: string }).targetOwnerLeaseGeneration,
    acceptedHighWater: (payload as { acceptedHighWater: string }).acceptedHighWater,
    sealId: (payload as { sealId: string }).sealId,
    acceptedJournalReference: (payload as { acceptedJournalReference: string }).acceptedJournalReference,
    acceptedJournalChecksumCrc32c:
      (payload as { acceptedJournalChecksumCrc32c: number }).acceptedJournalChecksumCrc32c,
    actorPacketTarget: (payload as { actorPacketTarget?: unknown }).actorPacketTarget
  };
}

export function encodeRemoteBoundSessionOwnershipAck(
  input: ZLinkRemoteBoundSessionOwnershipAck
): ZLinkRemoteBoundSessionOwnershipAck {
  return {
    actorId: input.actorId,
    actorGeneration: input.actorGeneration,
    actorOwnershipGeneration: input.actorOwnershipGeneration,
    bindingGeneration: input.bindingGeneration,
    targetOwnerLeaseGeneration: input.targetOwnerLeaseGeneration,
    acceptedHighWater: input.acceptedHighWater,
    sealId: input.sealId
  };
}

export function decodeRemoteBoundSessionOwnershipAck(
  payload: unknown
): ZLinkRemoteBoundSessionOwnershipAck {
  if (
    typeof payload !== 'object' ||
    payload === null ||
    typeof (payload as { actorId?: unknown }).actorId !== 'string' ||
    typeof (payload as { actorGeneration?: unknown }).actorGeneration !== 'string' ||
    typeof (payload as { actorOwnershipGeneration?: unknown }).actorOwnershipGeneration !== 'string' ||
    typeof (payload as { bindingGeneration?: unknown }).bindingGeneration !== 'string' ||
    typeof (payload as { targetOwnerLeaseGeneration?: unknown }).targetOwnerLeaseGeneration !== 'string' ||
    typeof (payload as { acceptedHighWater?: unknown }).acceptedHighWater !== 'string' ||
    typeof (payload as { sealId?: unknown }).sealId !== 'string'
  ) {
    throw new Error('Remote bound session ownership ACK is invalid.');
  }
  return {
    actorId: (payload as { actorId: string }).actorId,
    actorGeneration: (payload as { actorGeneration: string }).actorGeneration,
    actorOwnershipGeneration: (payload as { actorOwnershipGeneration: string }).actorOwnershipGeneration,
    bindingGeneration: (payload as { bindingGeneration: string }).bindingGeneration,
    targetOwnerLeaseGeneration: (payload as { targetOwnerLeaseGeneration: string }).targetOwnerLeaseGeneration,
    acceptedHighWater: (payload as { acceptedHighWater: string }).acceptedHighWater,
    sealId: (payload as { sealId: string }).sealId
  };
}

export function encodeRemoteBoundSessionSendPayload(input: {
  readonly actorId: string;
  readonly actorMeshName?: string;
  readonly actorNodeRid?: string;
  readonly actorNodeRidHex?: string;
  readonly actorGeneration?: string;
  readonly actorOwnershipGeneration?: string;
  readonly message: unknown;
  readonly boundPacketName?: string;
  readonly metadata: ReadonlyMap<string, string>;
  readonly flowId?: string;
  readonly flowOrigin?: import('../../contracts').ZLinkFlowOrigin;
  readonly actorPacketTarget?: unknown;
}): Record<string, unknown> {
  return {
    packetName: ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET,
    ...input,
    metadata: Object.fromEntries(input.metadata)
  };
}

export function encodeRemoteBoundSessionResponsePayload(input: {
  readonly actorId: string;
  readonly boundPacketName: string;
  readonly requestSeq: bigint;
  readonly message: unknown;
  readonly metadata: ReadonlyMap<string, string>;
  readonly compressPayload: boolean;
  readonly actorPacketTarget?: unknown;
}): Record<string, unknown> {
  return {
    packetName: ZLINK_REMOTE_BOUND_SESSION_RESPONSE_PACKET,
    ...input,
    requestSeq: input.requestSeq.toString(),
    metadata: Object.fromEntries(input.metadata)
  };
}

export function encodeRemoteBoundSessionErrorPayload(input: {
  readonly actorId: string;
  readonly boundPacketName: string;
  readonly requestSeq: bigint;
  readonly error: unknown;
  readonly metadata: ReadonlyMap<string, string>;
  readonly actorPacketTarget?: unknown;
}): Record<string, unknown> {
  return {
    packetName: ZLINK_REMOTE_BOUND_SESSION_ERROR_PACKET,
    ...input,
    requestSeq: input.requestSeq.toString(),
    error: input.error instanceof Error
      ? { code: input.error.constructor.name, message: input.error.message }
      : input.error,
    metadata: Object.fromEntries(input.metadata)
  };
}

export function decodeRemoteBoundSessionSendPayload(payload: unknown): {
  readonly actorId: string;
  readonly actorMeshName?: string;
  readonly actorNodeRid?: string;
  readonly actorNodeRidHex?: string;
  readonly actorGeneration?: string;
  readonly actorOwnershipGeneration?: string;
  readonly message: unknown;
  readonly boundPacketName?: string;
  readonly metadata?: Record<string, string>;
  readonly flowId?: string;
  readonly flowOrigin?: import('../../contracts').ZLinkFlowOrigin;
  readonly actorPacketTarget?: unknown;
} {
  if (
    typeof payload !== 'object' ||
    payload === null ||
    (payload as { packetName?: unknown }).packetName !== ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET ||
    typeof (payload as { actorId?: unknown }).actorId !== 'string'
  ) {
    throw new Error('Remote bound session send payload is invalid.');
  }
  return {
    actorId: (payload as { actorId: string }).actorId,
    actorMeshName: optionalString(payload, 'actorMeshName'),
    actorNodeRid: optionalString(payload, 'actorNodeRid'),
    actorNodeRidHex: optionalString(payload, 'actorNodeRidHex'),
    actorGeneration: optionalString(payload, 'actorGeneration'),
    actorOwnershipGeneration: optionalString(payload, 'actorOwnershipGeneration'),
    message: (payload as { message?: unknown }).message,
    boundPacketName: optionalString(payload, 'boundPacketName'),
    metadata: metadataRecordOf((payload as { metadata?: unknown }).metadata),
    flowId: optionalString(payload, 'flowId'),
    flowOrigin: optionalFlowOrigin(payload),
    actorPacketTarget: (payload as { actorPacketTarget?: unknown }).actorPacketTarget
  };
}

function optionalFlowOrigin(payload: object): import('../../contracts').ZLinkFlowOrigin | undefined {
  const value = (payload as { flowOrigin?: unknown }).flowOrigin;
  return value === 'Inbound' || value === 'Timer' || value === 'Application' || value === 'Lifecycle'
    ? value
    : undefined;
}

export function decodeRemoteBoundSessionResponsePayload(payload: unknown): {
  readonly actorId: string;
  readonly message: unknown;
  readonly boundPacketName: string;
  readonly requestSeq: bigint;
  readonly metadata?: Record<string, string>;
  readonly compressPayload: boolean;
  readonly actorPacketTarget?: unknown;
} {
  const decoded = decodeRemoteBoundSessionControlPayload(payload, ZLINK_REMOTE_BOUND_SESSION_RESPONSE_PACKET);
  return { ...decoded, message: (payload as { message?: unknown }).message };
}

export function decodeRemoteBoundSessionErrorPayload(payload: unknown): {
  readonly actorId: string;
  readonly error: unknown;
  readonly boundPacketName: string;
  readonly requestSeq: bigint;
  readonly metadata?: Record<string, string>;
  readonly actorPacketTarget?: unknown;
} {
  const decoded = decodeRemoteBoundSessionControlPayload(payload, ZLINK_REMOTE_BOUND_SESSION_ERROR_PACKET);
  return {
    actorId: decoded.actorId,
    error: (payload as { error?: unknown }).error,
    boundPacketName: decoded.boundPacketName,
    requestSeq: decoded.requestSeq,
    metadata: decoded.metadata,
    actorPacketTarget: decoded.actorPacketTarget
  };
}

export function streamMetadataMap(metadata: unknown): ReadonlyMap<string, string> {
  if (metadata instanceof Map) {
    return new Map(metadata);
  }
  const maybeValues = metadata as { values?: unknown } | undefined;
  if (maybeValues?.values instanceof Map) {
    return new Map(maybeValues.values);
  }
  return new Map();
}

function decodeRemoteBoundSessionControlPayload(payload: unknown, packetName: string): {
  readonly actorId: string;
  readonly boundPacketName: string;
  readonly requestSeq: bigint;
  readonly metadata?: Record<string, string>;
  readonly compressPayload: boolean;
  readonly actorPacketTarget?: unknown;
} {
  if (
    typeof payload !== 'object' ||
    payload === null ||
    (payload as { packetName?: unknown }).packetName !== packetName ||
    typeof (payload as { actorId?: unknown }).actorId !== 'string' ||
    typeof (payload as { boundPacketName?: unknown }).boundPacketName !== 'string' ||
    typeof (payload as { requestSeq?: unknown }).requestSeq !== 'string'
  ) {
    throw new Error('Remote bound session control payload is invalid.');
  }
  return {
    actorId: (payload as { actorId: string }).actorId,
    boundPacketName: (payload as { boundPacketName: string }).boundPacketName,
    requestSeq: BigInt((payload as { requestSeq: string }).requestSeq),
    metadata: metadataRecordOf((payload as { metadata?: unknown }).metadata),
    compressPayload: (payload as { compressPayload?: unknown }).compressPayload === true,
    actorPacketTarget: (payload as { actorPacketTarget?: unknown }).actorPacketTarget
  };
}

function metadataRecordOf(rawMetadata: unknown): Record<string, string> {
  const metadata: Record<string, string> = {};
  if (typeof rawMetadata === 'object' && rawMetadata !== null) {
    for (const [key, value] of Object.entries(rawMetadata)) {
      if (typeof value === 'string') {
        metadata[key] = value;
      }
    }
  }
  return metadata;
}

function optionalString(value: object, key: string): string | undefined {
  const field = (value as Record<string, unknown>)[key];
  return typeof field === 'string' ? field : undefined;
}
