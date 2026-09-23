import {
  RequiredZlinkStreamConnectorOptions,
  ZlinkStreamEncodedPayload,
  ZlinkStreamErrorCode,
  zlinkStreamJsonCodec,
  ZlinkStreamMessage,
  ZlinkStreamMessageKind,
  ZlinkStreamMetadata,
  ZlinkStreamMetadataMap,
  type ZlinkStreamRequestCall,
  type ZlinkStreamResultOf,
  type ZlinkStreamSendCall,
  type ZlinkStreamWaitCall
} from '../../Contracts';
import { connectorError, throwIfAborted, unwrapStreamError } from '../ZlinkStreamSupport';
import { validateName } from '../Protocol/ZlinkStreamPacketNameValidator';

interface ZlinkStreamConnectorSubmitter {
  readonly options: RequiredZlinkStreamConnectorOptions;
  sendEncoded(
    kind: ZlinkStreamMessageKind,
    name: string,
    payload: ZlinkStreamEncodedPayload,
    metadata: ZlinkStreamMetadata,
    compress: boolean,
    requestSeq: bigint | undefined,
    signal?: AbortSignal,
    correlationId?: string,
    actorSlot?: number
  ): Promise<void>;
  requestEncoded(
    name: string,
    payload: ZlinkStreamEncodedPayload,
    metadata: ZlinkStreamMetadata,
    compress: boolean,
    timeoutMs: number,
    signal?: AbortSignal,
    actorSlot?: number
  ): Promise<ZlinkStreamEncodedPayload>;
  waitForMessage<TPayload>(
    name: string,
    timeoutMs: number,
    predicate: (message: ZlinkStreamMessage<TPayload>) => boolean,
    signal?: AbortSignal
  ): Promise<ZlinkStreamMessage<TPayload> | undefined>;
}

class ZlinkStreamCallBuilderState {
  private executed = false;
  name: string | undefined;
  metadata: ZlinkStreamMetadata = ZlinkStreamMetadataMap.empty;
  timeoutMs: number | undefined;
  compress = false;

  constructor(
    name: string | undefined,
    readonly actorSlot?: number,
    private readonly validateActor?: () => void
  ) {
    this.name = name;
  }

  ensureNotExecuted(): void {
    if (this.executed) {
      throw connectorError(
        ZlinkStreamErrorCode.ValidationFailed,
        'Builder instances can be executed only once.'
      );
    }
    this.executed = true;
    this.validateActor?.();
  }

  resolveMessageName(): string {
    if (this.name === undefined) {
      throw connectorError(
        ZlinkStreamErrorCode.ValidationFailed,
        'Message name is required when the encoded stream payload has no message type.'
      );
    }
    return this.name;
  }
}

export class ZlinkStreamSendBuilder implements ZlinkStreamSendCall {
  private readonly state: ZlinkStreamCallBuilderState;

  constructor(
    private readonly connector: ZlinkStreamConnectorSubmitter,
    name: string | undefined,
    private readonly payload: ZlinkStreamEncodedPayload,
    actorSlot?: number,
    validateActor?: () => void
  ) {
    this.state = new ZlinkStreamCallBuilderState(name, actorSlot, validateActor);
  }

  packetName(name: string): this {
    validateName(name);
    this.state.name = name;
    return this;
  }

  metadata(key: string, value: string): this;
  metadata(metadata: ZlinkStreamMetadata): this;
  metadata(keyOrMetadata: string | ZlinkStreamMetadata, value?: string): this {
    this.state.metadata =
      typeof keyOrMetadata === 'string'
        ? this.state.metadata.with(keyOrMetadata, value ?? '')
        : keyOrMetadata;
    return this;
  }

  compress(): this {
    this.state.compress = true;
    return this;
  }

  async submit(signal?: AbortSignal): Promise<void> {
    throwIfAborted(signal);
    this.state.ensureNotExecuted();
    await this.connector.sendEncoded(
      ZlinkStreamMessageKind.Send,
      this.state.resolveMessageName(),
      this.payload,
      this.state.metadata,
      this.state.compress,
      undefined,
      signal,
      undefined,
      this.state.actorSlot
    );
  }
}

export class ZlinkStreamRequestBuilder implements ZlinkStreamRequestCall {
  private readonly state: ZlinkStreamCallBuilderState;

  constructor(
    private readonly connector: ZlinkStreamConnectorSubmitter,
    name: string | undefined,
    private readonly payload: ZlinkStreamEncodedPayload,
    actorSlot?: number,
    validateActor?: () => void
  ) {
    this.state = new ZlinkStreamCallBuilderState(name, actorSlot, validateActor);
  }

  packetName(name: string): this {
    validateName(name);
    this.state.name = name;
    return this;
  }

  metadata(key: string, value: string): this;
  metadata(metadata: ZlinkStreamMetadata): this;
  metadata(keyOrMetadata: string | ZlinkStreamMetadata, value?: string): this {
    this.state.metadata =
      typeof keyOrMetadata === 'string'
        ? this.state.metadata.with(keyOrMetadata, value ?? '')
        : keyOrMetadata;
    return this;
  }

  timeout(timeoutMs: number): this {
    this.state.timeoutMs = timeoutMs;
    return this;
  }

  compress(): this {
    this.state.compress = true;
    return this;
  }

  submit<TReply = unknown>(signal?: AbortSignal): Promise<TReply>;
  submit(callback: (result: ZlinkStreamResultOf<ZlinkStreamEncodedPayload>) => void): void;
  submit<TReply = unknown>(
    signalOrCallback?:
      AbortSignal | ((result: ZlinkStreamResultOf<ZlinkStreamEncodedPayload>) => void)
  ): Promise<TReply> | void {
    this.state.ensureNotExecuted();
    const operation = this.connector.requestEncoded(
      this.state.resolveMessageName(),
      this.payload,
      this.state.metadata,
      this.state.compress,
      this.state.timeoutMs ?? this.connector.options.requestTimeoutMs,
      typeof signalOrCallback === 'function' ? undefined : signalOrCallback,
      this.state.actorSlot
    );
    if (typeof signalOrCallback === 'function') {
      operation.then(
        (value) => signalOrCallback({ isSuccess: true, value }),
        (error) => signalOrCallback({ isSuccess: false, error: unwrapStreamError(error) })
      );
      return;
    }
    return operation.then((value) =>
      (this.connector.options.codec ?? zlinkStreamJsonCodec).decode<TReply>(value)
    );
  }

  submitEncoded(signal?: AbortSignal): Promise<ZlinkStreamEncodedPayload> {
    this.state.ensureNotExecuted();
    return this.connector.requestEncoded(
      this.state.resolveMessageName(),
      this.payload,
      this.state.metadata,
      this.state.compress,
      this.state.timeoutMs ?? this.connector.options.requestTimeoutMs,
      signal,
      this.state.actorSlot
    );
  }
}

export class ZlinkStreamWaitBuilder<
  TPayload = ZlinkStreamEncodedPayload
> implements ZlinkStreamWaitCall<TPayload> {
  private executed = false;
  private timeoutMs: number | undefined;
  private predicate: (message: ZlinkStreamMessage<TPayload>) => boolean = () => true;

  constructor(
    private readonly connector: ZlinkStreamConnectorSubmitter,
    private readonly name: string
  ) {}

  where(predicate: (message: ZlinkStreamMessage<TPayload>) => boolean): this {
    this.ensureConfigurable();
    this.predicate = predicate;
    return this;
  }

  timeout(timeoutMs: number): this {
    this.ensureConfigurable();
    this.timeoutMs = timeoutMs;
    return this;
  }

  async submit(signal?: AbortSignal): Promise<ZlinkStreamMessage<TPayload>> {
    this.markExecuted();
    const timeoutMs = this.timeoutMs ?? this.connector.options.waitTimeoutMs;
    const message = await this.connector.waitForMessage(
      this.name,
      timeoutMs,
      this.predicate,
      signal
    );
    if (message === undefined) {
      // Spec stream-connector 32 §10.1.1: nothing arriving inside the window
      // is a violated observation, not a request that got no reply.
      throw connectorError(
        ZlinkStreamErrorCode.ValidationFailed,
        `No '${this.name}' message arrived within ${timeoutMs}ms.`
      );
    }
    return message;
  }

  private ensureConfigurable(): void {
    if (this.executed) {
      throw connectorError(
        ZlinkStreamErrorCode.ValidationFailed,
        'Builder instances can be executed only once.'
      );
    }
  }

  private markExecuted(): void {
    this.ensureConfigurable();
    this.executed = true;
  }
}
