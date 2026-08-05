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
  ZlinkStreamInboundObservation,
  ZlinkStreamMessage
} from './ZlinkStreamModels';
import type { ZlinkStreamCloseReason, ZlinkStreamConnectionState } from './ZlinkStreamEnums';

export interface ZlinkStreamConnector {
  readonly isConnected: boolean;
  readonly state: ZlinkStreamConnectionState;
  readonly closeReason?: ZlinkStreamCloseReason;
  readonly options: RequiredZlinkStreamConnectorOptions;
  readonly pendingDispatchCount: number;
  onErrorReceived(handler: (error: ZlinkStreamError, signal?: AbortSignal) => Promise<void> | void): Disposable;
  onDisconnected(handler: (signal?: AbortSignal) => Promise<void> | void): Disposable;
  onConnectionStateChanged(handler: (change: ZlinkStreamConnectionStateChanged, signal?: AbortSignal) => Promise<void> | void): Disposable;
  connect(signal?: AbortSignal): Promise<void>;
  close(signal?: AbortSignal): Promise<void>;
  dispatch(signal?: AbortSignal): Promise<void>;
  send(payload: unknown, messageType?: Function): ZlinkStreamSendCall;
  request(payload: unknown, messageType?: Function): ZlinkStreamRequestCall;
  observeInbound(
    observer: (observation: ZlinkStreamInboundObservation, signal?: AbortSignal) => Promise<void> | void
  ): Disposable;
  on<TPayload = ZlinkStreamEncodedPayload>(
    name: string,
    handler: (message: ZlinkStreamMessage<TPayload>, signal?: AbortSignal) => Promise<void> | void,
    messageType?: Function
  ): Disposable;
  waitFor<TPayload = ZlinkStreamEncodedPayload>(name: string): ZlinkStreamWaitCall<TPayload>;
  expectNone<TPayload = ZlinkStreamEncodedPayload>(name: string): ZlinkStreamExpectNoneCall<TPayload>;
  waitForSequence<TPayload = ZlinkStreamEncodedPayload>(name: string): ZlinkStreamSequenceCall<TPayload>;
}
