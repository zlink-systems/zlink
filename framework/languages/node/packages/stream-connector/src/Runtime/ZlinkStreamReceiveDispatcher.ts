import type { ZlinkStreamConnection } from '../Contracts';
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
   * @param connectionForSend Resolves the connection a reply belongs on at the
   *   moment it is written. The captured `connection` is the one the batch was
   *   read from, which a reconnect may already have replaced.
   */
  async readAndDispatch(
    connection: ZlinkStreamConnection | undefined,
    signal?: AbortSignal,
    isCurrent?: () => boolean,
    connectionForSend?: () => ZlinkStreamConnection
  ): Promise<ZlinkStreamReceiveResult> {
    if (connection?.read === undefined) {
      return { available: false, inbound: false };
    }
    const frameBytes = await connection.read(signal);
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
      throw cause;
    }
    for (const frame of frames) {
      if (isCurrent !== undefined && !isCurrent()) {
        break;
      }
      try {
        await this.dispatch(connection, frame.header, frame.payload, signal, connectionForSend);
      } catch (cause) {
        if (
          frame.header.kind === ZlinkStreamMessageKind.Control &&
          frame.header.name === ZLINK_STREAM_HEARTBEAT_PING
        ) {
          throw cause;
        }
        const error = toStreamError(
          cause,
          ZlinkStreamErrorCode.FrameDecodeFailed,
          'Frame dispatch failed.'
        );
        if (error.code === ZlinkStreamErrorCode.FrameDecodeFailed) {
          throw cause;
        }
        await this.events.publishError(error, signal);
      }
    }
    return { available: true, inbound: true };
  }

  private async dispatch(
    connection: ZlinkStreamConnection,
    header: ZlinkStreamHeader,
    payload: Uint8Array,
    signal: AbortSignal | undefined,
    connectionForSend?: () => ZlinkStreamConnection
  ): Promise<void> {
    const actor =
      header.actorSlot === undefined ? undefined : this.actors.resolve(header.actorSlot);
    if (header.kind === ZlinkStreamMessageKind.Response && header.requestSeq !== undefined) {
      try {
        if (
          !this.pendingRequests.resolve(
            header.requestSeq,
            {
              codec: header.codec,
              payload: this.protocol.decodePayload(header, payload)
            },
            header.metadata
          )
        ) {
          await this.events.publishError(
            {
              code: ZlinkStreamErrorCode.FrameDecodeFailed,
              message: `Response request sequence '${header.requestSeq}' has no pending request.`
            },
            signal
          );
        }
      } catch (cause) {
        const decodeError = toStreamError(
          cause,
          ZlinkStreamErrorCode.DecompressionFailed,
          'Decompression failed.'
        );
        if (!this.pendingRequests.reject(header.requestSeq, decodeError)) {
          await this.events.publishError(decodeError, signal);
        }
      }
      return;
    }
    if (header.kind === ZlinkStreamMessageKind.Error && header.requestSeq !== undefined) {
      try {
        const remoteError = decodeRemoteError(this.protocol, header, payload);
        if (!this.pendingRequests.reject(header.requestSeq, remoteError)) {
          await this.events.publishError(remoteError, signal);
        }
      } catch (cause) {
        const decodeError = toStreamError(
          cause,
          ZlinkStreamErrorCode.FrameDecodeFailed,
          'Remote error payload is invalid.'
        );
        if (!this.pendingRequests.reject(header.requestSeq, decodeError)) {
          await this.events.publishError(decodeError, signal);
        }
      }
      return;
    }
    if (header.kind === ZlinkStreamMessageKind.Error) {
      await this.events.publishError(decodeRemoteError(this.protocol, header, payload), signal);
      return;
    }
    if (header.kind === ZlinkStreamMessageKind.Control) {
      await this.dispatchControl(connection, header, payload, signal, connectionForSend);
      return;
    }
    if (header.kind === ZlinkStreamMessageKind.Send) {
      this.receivedMessages.enqueue(
        {
          name: header.name,
          metadata: header.metadata,
          payload: { codec: header.codec, payload: this.protocol.decodePayload(header, payload) },
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
    signal?: AbortSignal,
    connectionForSend?: () => ZlinkStreamConnection
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
      try {
        // The pong answers on the connection the connector holds now, not on
        // the one this batch was read from: a reconnect inside an earlier
        // frame's await would otherwise write it to a replaced transport.
        await this.frameSender.sendControl(
          connectionForSend?.() ?? connection,
          ZLINK_STREAM_HEARTBEAT_PONG,
          signal
        );
      } catch (cause) {
        throw connectorError(
          ZlinkStreamErrorCode.SendFailed,
          cause instanceof Error ? cause.message : 'Heartbeat pong send failed.'
        );
      }
      return;
    }
    if (header.name !== ZLINK_STREAM_HEARTBEAT_PONG) {
      throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, 'Unknown control packet.');
    }
  }
}

function decodeRemoteError(
  protocol: ZlinkStreamFrameProtocol,
  header: ZlinkStreamHeader,
  payload: Uint8Array
): { readonly code: ZlinkStreamErrorCode; readonly message: string; readonly cause: unknown } {
  const decodedPayload = protocol.decodePayload(header, payload);
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
