import type { ZlinkStreamMetadata } from '../ZlinkStreamMetadata';
import type {
  ZlinkStreamEncodedPayload,
  ZlinkStreamFlow,
  ZlinkStreamMessage,
  ZlinkStreamResultOf
} from '../ZlinkStreamModels';

export interface ZlinkStreamSendCall {
  packetName(name: string): ZlinkStreamSendCall;
  metadata(key: string, value: string): ZlinkStreamSendCall;
  metadata(metadata: ZlinkStreamMetadata): ZlinkStreamSendCall;
  compress(): ZlinkStreamSendCall;
  flowFrom(flow: ZlinkStreamFlow): ZlinkStreamSendCall;
  submit(): Promise<void>;
}

export interface ZlinkStreamRequestCall {
  packetName(name: string): ZlinkStreamRequestCall;
  metadata(key: string, value: string): ZlinkStreamRequestCall;
  metadata(metadata: ZlinkStreamMetadata): ZlinkStreamRequestCall;
  timeout(timeoutMs: number): ZlinkStreamRequestCall;
  compress(): ZlinkStreamRequestCall;
  flowFrom(flow: ZlinkStreamFlow): ZlinkStreamRequestCall;
  submit<TReply = unknown>(signal?: AbortSignal): Promise<TReply>;
  submitEncoded(signal?: AbortSignal): Promise<ZlinkStreamEncodedPayload>;
  submit(callback: (result: ZlinkStreamResultOf<ZlinkStreamEncodedPayload>) => void): void;
}

export interface ZlinkStreamWaitCall<TPayload = ZlinkStreamEncodedPayload> {
  where(predicate: (message: ZlinkStreamMessage<TPayload>) => boolean): ZlinkStreamWaitCall<TPayload>;
  timeout(timeoutMs: number): ZlinkStreamWaitCall<TPayload>;
  submit(signal?: AbortSignal): Promise<ZlinkStreamMessage<TPayload>>;
}

export interface ZlinkStreamExpectNoneCall<TPayload = ZlinkStreamEncodedPayload> {
  within(windowMs: number): ZlinkStreamExpectNoneCall<TPayload>;
  run(signal?: AbortSignal): Promise<void>;
}

export interface ZlinkStreamSequenceCall<TPayload = ZlinkStreamEncodedPayload> {
  expect(predicate: (payload: TPayload) => boolean): ZlinkStreamSequenceCall<TPayload>;
  timeout(timeoutMs: number): ZlinkStreamSequenceCall<TPayload>;
  run(signal?: AbortSignal): Promise<readonly TPayload[]>;
}
