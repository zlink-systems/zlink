import type { Disposable } from './ZlinkStreamInterfaces';
import type { RequiredZlinkStreamConnectorOptions } from './ZlinkStreamConnectorOptions';
import type {
  ZlinkStreamRequestCall,
  ZlinkStreamSendCall,
  ZlinkStreamExpectNoneCall,
  ZlinkStreamSequenceCall,
  ZlinkStreamWaitCall
} from './Calls/ZlinkStreamCalls';
import type {
  ZlinkStreamConnectionStateChanged,
  ZlinkStreamEncodedPayload,
  ZlinkStreamError,
  ZlinkStreamMessage,
  ZlinkStreamRequestSendingContext,
  ZlinkStreamReplyReceivedContext
} from './ZlinkStreamModels';
import type { ZlinkStreamCloseReason, ZlinkStreamConnectionState } from './ZlinkStreamEnums';
import type { ZlinkStreamActor } from './ZlinkStreamActor';

export interface ZlinkStreamConnector {
  readonly isConnected: boolean;
  readonly state: ZlinkStreamConnectionState;
  readonly closeReason?: ZlinkStreamCloseReason;
  readonly options: RequiredZlinkStreamConnectorOptions;
  readonly pendingDispatchCount: number;
  readonly actors: readonly ZlinkStreamActor[];
  actor(actorId: string): ZlinkStreamActor | undefined;
  onActorBound(
    handler: (actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void
  ): Disposable;
  onActorUnbound(
    handler: (actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void
  ): Disposable;
  /**
   * Number of packets received under `name` on the current connection (spec
   * stream-connector 32 §10). Counts arrivals, so consuming a message through
   * a handler or a wait surface never lowers it, and the dispatch mode does
   * not change it. Restarts at 0 every time a connection is established.
   */
  receivedCount(name: string): number;
  onErrorReceived(
    handler: (error: ZlinkStreamError, signal?: AbortSignal) => Promise<void> | void
  ): Disposable;
  onDisconnected(handler: (signal?: AbortSignal) => Promise<void> | void): Disposable;
  onConnectionStateChanged(
    handler: (
      change: ZlinkStreamConnectionStateChanged,
      signal?: AbortSignal
    ) => Promise<void> | void
  ): Disposable;
  onRequestSending(handler: (context: ZlinkStreamRequestSendingContext) => void): Disposable;
  onReplyReceived(
    handler: (
      context: ZlinkStreamReplyReceivedContext,
      signal?: AbortSignal
    ) => Promise<void> | void
  ): Disposable;
  connect(signal?: AbortSignal): Promise<void>;
  close(signal?: AbortSignal): Promise<void>;
  dispatch(signal?: AbortSignal): Promise<void>;
  send(payload: unknown, messageType?: Function): ZlinkStreamSendCall;
  request(payload: unknown, messageType?: Function): ZlinkStreamRequestCall;
  on<TPayload = ZlinkStreamEncodedPayload>(
    nameOrType: string | Function,
    handler: (message: ZlinkStreamMessage<TPayload>, signal?: AbortSignal) => Promise<void> | void,
    messageType?: Function
  ): Disposable;
  /**
   * Wait surfaces (spec stream-connector 32 §10.1). Each takes the packet name
   * the caller states explicitly, or the payload constructor the options'
   * `nameResolver` turns into that name — TypeScript types are erased at
   * runtime, so the type-driven path takes a constructor value.
   */
  waitFor<TPayload = ZlinkStreamEncodedPayload>(
    nameOrType: string | Function
  ): ZlinkStreamWaitCall<TPayload>;
  expectNone<TPayload = ZlinkStreamEncodedPayload>(
    nameOrType: string | Function
  ): ZlinkStreamExpectNoneCall<TPayload>;
  waitForSequence<TPayload = ZlinkStreamEncodedPayload>(
    nameOrType: string | Function
  ): ZlinkStreamSequenceCall<TPayload>;
}
