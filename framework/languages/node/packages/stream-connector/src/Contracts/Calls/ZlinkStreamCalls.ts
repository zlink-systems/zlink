import type { ZlinkStreamMetadata } from '../ZlinkStreamMetadata';
import type {
  ZlinkStreamEncodedPayload,
  ZlinkStreamMessage,
  ZlinkStreamResultOf
} from '../ZlinkStreamModels';

export interface ZlinkStreamSendCall {
  packetName(name: string): ZlinkStreamSendCall;
  metadata(key: string, value: string): ZlinkStreamSendCall;
  metadata(metadata: ZlinkStreamMetadata): ZlinkStreamSendCall;
  compress(): ZlinkStreamSendCall;
  submit(): Promise<void>;
}

export interface ZlinkStreamRequestCall {
  packetName(name: string): ZlinkStreamRequestCall;
  metadata(key: string, value: string): ZlinkStreamRequestCall;
  metadata(metadata: ZlinkStreamMetadata): ZlinkStreamRequestCall;
  timeout(timeoutMs: number): ZlinkStreamRequestCall;
  compress(): ZlinkStreamRequestCall;
  submit<TReply = unknown>(signal?: AbortSignal): Promise<TReply>;
  submitEncoded(signal?: AbortSignal): Promise<ZlinkStreamEncodedPayload>;
  submit(callback: (result: ZlinkStreamResultOf<ZlinkStreamEncodedPayload>) => void): void;
}

export interface ZlinkStreamWaitCall<TPayload = ZlinkStreamEncodedPayload> {
  where(
    predicate: (message: ZlinkStreamMessage<TPayload>) => boolean
  ): ZlinkStreamWaitCall<TPayload>;
  timeout(timeoutMs: number): ZlinkStreamWaitCall<TPayload>;
  submit(signal?: AbortSignal): Promise<ZlinkStreamMessage<TPayload>>;
}

export interface ZlinkStreamExpectNoneCall<TPayload = ZlinkStreamEncodedPayload> {
  within(windowMs: number): ZlinkStreamExpectNoneCall<TPayload>;
  run(signal?: AbortSignal): Promise<void>;
}

/**
 * Spec stream-connector 32 §10.1: the predicates and the returned values are
 * messages, not payloads. A payload-only predicate cannot read the packet name
 * or the metadata, so `TPayload` is the type of the payload the message carries
 * and never the value handed to `expect(...)`.
 */
export interface ZlinkStreamSequenceCall<TPayload = ZlinkStreamEncodedPayload> {
  expect(
    predicate: (message: ZlinkStreamMessage<TPayload>) => boolean
  ): ZlinkStreamSequenceCall<TPayload>;
  timeout(timeoutMs: number): ZlinkStreamSequenceCall<TPayload>;
  run(signal?: AbortSignal): Promise<readonly ZlinkStreamMessage<TPayload>[]>;
}
