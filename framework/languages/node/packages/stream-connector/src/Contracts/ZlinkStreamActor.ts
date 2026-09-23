import type { Disposable } from './ZlinkStreamInterfaces';
import type { ZlinkStreamRequestCall, ZlinkStreamSendCall } from './Calls/ZlinkStreamCalls';
import type { ZlinkStreamEncodedPayload, ZlinkStreamMessage } from './ZlinkStreamModels';

export interface ZlinkStreamActor {
  readonly actorId: string;
  readonly isBound: boolean;
  send(payload: unknown, messageType?: Function): ZlinkStreamSendCall;
  request(payload: unknown, messageType?: Function): ZlinkStreamRequestCall;
  on<TPayload = ZlinkStreamEncodedPayload>(
    nameOrType: string | Function,
    handler: (message: ZlinkStreamMessage<TPayload>, signal?: AbortSignal) => Promise<void> | void,
    messageType?: Function
  ): Disposable;
}
