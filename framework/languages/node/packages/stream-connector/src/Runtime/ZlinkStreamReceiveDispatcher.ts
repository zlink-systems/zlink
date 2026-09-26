import type { ZlinkStreamConnection, ZlinkStreamError } from '../Contracts';
import { ZlinkStreamErrorCode, ZlinkStreamMessageKind } from '../Contracts';
import type { ZlinkStreamHeader } from '../Contracts/ZlinkStreamModels';
import type { ZlinkStreamFrameProtocol } from './Protocol/ZlinkStreamFrameProtocol';
import {
  ZLINK_STREAM_HEARTBEAT_PING,
  ZLINK_STREAM_HEARTBEAT_PONG
} from './Protocol/ZlinkStreamFrameProtocol';
import type { ZlinkStreamConnectorEvents } from './ZlinkStreamConnectorEvents';
import type { ZlinkStreamFrameSender } from './ZlinkStreamFrameSender';
import type { ZlinkStreamPendingRequests } from './ZlinkStreamPendingRequests';
import type { ZlinkStreamReceivedMessages } from './ZlinkStreamReceivedMessages';
import { connectorError, toStreamError, utf8Decode } from './ZlinkStreamSupport';
import { decodeSessionClosing, ZLINK_SESSION_CLOSING } from './Protocol/ZlinkSessionClosing';
import type { ZlinkStreamCloseReason } from '../Contracts';
import { zlinkStreamActorBinding, type ZlinkStreamActors } from './ZlinkStreamActors';

/**
 * Spec stream-connector 32 §9: the one mapping from the error that ended a
 * connection to its close reason. A frame or header the connector cannot
 * decode and a payload over the receive limit are protocol violations; every
 * other failure — a transport read or write — is a transport error.
 */
export function closeReasonFor(error: ZlinkStreamError): ZlinkStreamCloseReason {
  return error.code === ZlinkStreamErrorCode.FrameDecodeFailed ||
    error.code === ZlinkStreamErrorCode.FrameTooLarge
    ? 'ProtocolError'
    : 'TransportError';
}

/** A receive failure that ends the connection; {@link closeReasonFor} names its reason. */
export class ZlinkStreamConnectionEnd extends Error {
  constructor(readonly error: ZlinkStreamError) {
    super(error.message);
  }
}

export interface ZlinkStreamReceiveResult {
  readonly available: boolean;
  readonly inbound: boolean;
}

export class ZlinkStreamReceiveDispatcher {
  constructor(
    private readonly protocol: ZlinkStreamFrameProtocol,
    private readonly pendingRequests: ZlinkStreamPendingRequests,
    private readonly receivedMessages: ZlinkStreamReceivedMessages,
    private readonly frameSender: ZlinkStreamFrameSender,
    private readonly events: ZlinkStreamConnectorEvents,
    private readonly actors: ZlinkStreamActors,
    private readonly serverClosing?: (reason: ZlinkStreamCloseReason) => Promise<void>
  ) {}

  /**
   * @param isCurrent Tells whether the connection this batch was read from is
   *   still the connector's connection. It is asked again at the head of every
   *   frame, not only after the read: `dispatch` awaits application code —
   *   registered handlers in `Immediate`, the error handler on a failed frame —
   *   and a disconnect and reconnect can complete inside that await. Without
   *   the recheck the rest of a dead connection's batch lands on the new
   *   connection's state, and the §10 arrival counts, which start at 0 when a
   *   connection is established, take the stale frames on top.
   */
  async readAndDispatch(
    connection: ZlinkStreamConnection | undefined,
    signal?: AbortSignal,
    isCurrent?: () => boolean
  ): Promise<ZlinkStreamReceiveResult> {
    if (connection?.read === undefined) {
      return { available: false, inbound: false };
    }
    let frameBytes: Uint8Array | undefined;
    try {
      frameBytes = await connection.read(signal);
    } catch (cause) {
      if (signal?.aborted === true) throw cause;
      // A transport may report a message it could not decode as a frame; any
      // other read failure is the transport's.
      throw new ZlinkStreamConnectionEnd(
        toStreamError(cause, ZlinkStreamErrorCode.Disconnected, 'Transport read failed.')
      );
    }
    if (isCurrent !== undefined && !isCurrent()) {
      return { available: false, inbound: false };
    }
    if (frameBytes === undefined) {
      return { available: false, inbound: false };
    }
    let frames: ReturnType<ZlinkStreamFrameProtocol['decodeFrames']>;
    try {
      frames = this.protocol.decodeFrames(frameBytes);
    } catch (cause) {
      throw new ZlinkStreamConnectionEnd(
        toStreamError(cause, ZlinkStreamErrorCode.FrameDecodeFailed, 'Frame decode failed.')
      );
    }
    for (const frame of frames) {
      if (isCurrent !== undefined && !isCurrent()) {
        break;
      }
      try {
        await this.dispatch(connection, frame.header, frame.payload, signal);
      } catch (cause) {
        if (cause instanceof ZlinkStreamConnectionEnd) throw cause;
        const error = toStreamError(
          cause,
          ZlinkStreamErrorCode.FrameDecodeFailed,
          'Frame dispatch failed.'
        );
        if (error.code === ZlinkStreamErrorCode.FrameDecodeFailed) {
          throw new ZlinkStreamConnectionEnd(error);
        }
        this.events.publishError(error, signal);
      }
    }
    return { available: true, inbound: true };
  }

  /**
   * Spec stream-connector 32 §4.7 and §9: a decompressed payload over the
   * receive limit is `FrameTooLarge` and ends the connection as a protocol
   * error, like a wire payload over the limit. A payload that does not
   * decompress fails only its packet.
   */
  private decodePayload(header: ZlinkStreamHeader, payload: Uint8Array): Uint8Array {
    try {
      return this.protocol.decodePayload(header, payload);
    } catch (cause) {
      const error = toStreamError(
        cause,
        ZlinkStreamErrorCode.DecompressionFailed,
        'Decompression failed.'
      );
      if (error.code === ZlinkStreamErrorCode.FrameTooLarge) {
        throw new ZlinkStreamConnectionEnd(error);
      }
      throw cause;
    }
  }

  private async dispatch(
    connection: ZlinkStreamConnection,
    header: ZlinkStreamHeader,
    payload: Uint8Array,
    signal: AbortSignal | undefined
  ): Promise<void> {
    const actor =
      header.actorSlot === undefined ? undefined : this.actors.resolve(header.actorSlot);
    if (header.kind === ZlinkStreamMessageKind.Response && header.requestSeq !== undefined) {
      try {
        this.pendingRequests.resolve(
          header.requestSeq,
          () => ({
            codec: header.codec,
            payload: this.decodePayload(header, payload)
          }),
          header.metadata
        );
      } catch (cause) {
        if (cause instanceof ZlinkStreamConnectionEnd) throw cause;
        const decodeError = toStreamError(
          cause,
          ZlinkStreamErrorCode.DecompressionFailed,
          'Decompression failed.'
        );
        if (!this.pendingRequests.reject(header.requestSeq, decodeError)) {
          this.events.publishError(decodeError, signal);
        }
      }
      return;
    }
    if (header.kind === ZlinkStreamMessageKind.Error && header.requestSeq !== undefined) {
      try {
        const remoteError = decodeRemoteError(this.decodePayload(header, payload));
        if (!this.pendingRequests.reject(header.requestSeq, remoteError)) {
          this.events.publishError(remoteError, signal);
        }
      } catch (cause) {
        if (cause instanceof ZlinkStreamConnectionEnd) throw cause;
        const decodeError = toStreamError(
          cause,
          ZlinkStreamErrorCode.FrameDecodeFailed,
          'Remote error payload is invalid.'
        );
        if (!this.pendingRequests.reject(header.requestSeq, decodeError)) {
          this.events.publishError(decodeError, signal);
        }
      }
      return;
    }
    if (header.kind === ZlinkStreamMessageKind.Error) {
      this.events.publishError(decodeRemoteError(this.decodePayload(header, payload)), signal);
      return;
    }
    if (header.kind === ZlinkStreamMessageKind.Control) {
      await this.dispatchControl(connection, header, payload, signal);
      return;
    }
    if (header.kind === ZlinkStreamMessageKind.Send) {
      this.receivedMessages.enqueue(
        {
          name: header.name,
          metadata: header.metadata,
          payload: { codec: header.codec, payload: this.decodePayload(header, payload) },
          actorId: actor?.actorId,
          [zlinkStreamActorBinding]: actor
        } as import('../Contracts').ZlinkStreamMessage<
          import('../Contracts').ZlinkStreamEncodedPayload
        >,
        signal
      );
    }
  }

  private async dispatchControl(
    connection: ZlinkStreamConnection,
    header: ZlinkStreamHeader,
    payload: Uint8Array,
    signal?: AbortSignal
  ): Promise<void> {
    if (this.actors.processControl(header.name, payload, signal)) {
      return;
    }
    if (header.name === ZLINK_SESSION_CLOSING) {
      const closing = decodeSessionClosing(payload);
      await this.serverClosing?.(closing.closeReason);
      return;
    }
    if (payload.length !== 0) {
      throw connectorError(
        ZlinkStreamErrorCode.FrameDecodeFailed,
        'Control packet payload must be empty.'
      );
    }
    if (header.name === ZLINK_STREAM_HEARTBEAT_PING) {
      // The pong answers on the connection the ping was read from. A failed
      // write ends that connection through the write queue (§9); a connection
      // that has already ended writes nothing.
      await this.frameSender.sendControl(connection, ZLINK_STREAM_HEARTBEAT_PONG);
      return;
    }
    if (header.name !== ZLINK_STREAM_HEARTBEAT_PONG) {
      throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, 'Unknown control packet.');
    }
  }
}

function decodeRemoteError(decodedPayload: Uint8Array): {
  readonly code: ZlinkStreamErrorCode;
  readonly message: string;
  readonly cause: unknown;
} {
  let decoded: unknown;
  try {
    decoded = JSON.parse(utf8Decode(decodedPayload));
  } catch (cause) {
    throw connectorError(
      ZlinkStreamErrorCode.FrameDecodeFailed,
      'Remote error payload must be a JSON object.',
      cause
    );
  }
  if (
    decoded === null ||
    typeof decoded !== 'object' ||
    Array.isArray(decoded) ||
    typeof (decoded as { code?: unknown }).code !== 'string' ||
    typeof (decoded as { message?: unknown }).message !== 'string'
  ) {
    throw connectorError(
      ZlinkStreamErrorCode.FrameDecodeFailed,
      'Remote error payload must contain string code and message fields.'
    );
  }
  const remote = decoded as { readonly code: string; readonly message: string };
  return { code: ZlinkStreamErrorCode.RemoteError, message: remote.message, cause: remote };
}
