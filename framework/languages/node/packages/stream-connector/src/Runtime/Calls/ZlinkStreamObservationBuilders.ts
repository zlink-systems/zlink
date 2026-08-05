import {
  ZlinkStreamErrorCode,
  type RequiredZlinkStreamConnectorOptions,
  type ZlinkStreamExpectNoneCall,
  type ZlinkStreamMessage,
  type ZlinkStreamSequenceCall
} from '../../Contracts';
import { connectorError, unwrapStreamError } from '../ZlinkStreamSupport';

export interface ZlinkStreamMessageWaiter {
  readonly options: RequiredZlinkStreamConnectorOptions;
  waitForMessage<TPayload>(
    name: string,
    timeoutMs: number,
    predicate: (message: ZlinkStreamMessage<TPayload>) => boolean,
    signal?: AbortSignal
  ): Promise<ZlinkStreamMessage<TPayload>>;
}

export class ZlinkStreamExpectNoneBuilder<TPayload> implements ZlinkStreamExpectNoneCall<TPayload> {
  private windowMs: number | undefined;
  private executed = false;

  constructor(
    private readonly connector: ZlinkStreamMessageWaiter,
    private readonly name: string
  ) {}

  within(windowMs: number): this {
    this.ensureConfigurable();
    validateTimeout(windowMs);
    this.windowMs = windowMs;
    return this;
  }

  async run(signal?: AbortSignal): Promise<void> {
    this.markExecuted();
    if (this.windowMs === undefined) {
      throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'expectNone requires within(windowMs).');
    }
    try {
      await this.connector.waitForMessage<TPayload>(this.name, this.windowMs, () => true, signal);
    } catch (error) {
      if (unwrapStreamError(error).code === ZlinkStreamErrorCode.RequestTimeout) {
        return;
      }
      throw error;
    }
    throw connectorError(
      ZlinkStreamErrorCode.ValidationFailed,
      `Expected no '${this.name}' message within ${this.windowMs}ms.`
    );
  }

  private ensureConfigurable(): void {
    if (this.executed) {
      throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Builder instances can be executed only once.');
    }
  }

  private markExecuted(): void {
    this.ensureConfigurable();
    this.executed = true;
  }
}

export class ZlinkStreamSequenceBuilder<TPayload> implements ZlinkStreamSequenceCall<TPayload> {
  private readonly predicates: Array<(payload: TPayload) => boolean> = [];
  private timeoutMs: number | undefined;
  private executed = false;

  constructor(
    private readonly connector: ZlinkStreamMessageWaiter,
    private readonly name: string
  ) {}

  expect(predicate: (payload: TPayload) => boolean): this {
    this.ensureConfigurable();
    this.predicates.push(predicate);
    return this;
  }

  timeout(timeoutMs: number): this {
    this.ensureConfigurable();
    validateTimeout(timeoutMs);
    this.timeoutMs = timeoutMs;
    return this;
  }

  async run(signal?: AbortSignal): Promise<readonly TPayload[]> {
    this.markExecuted();
    if (this.predicates.length === 0) {
      throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'waitForSequence requires at least one expectation.');
    }
    const timeoutMs = this.timeoutMs ?? this.connector.options.waitTimeoutMs;
    const deadline = Date.now() + timeoutMs;
    const payloads: TPayload[] = [];
    for (const predicate of this.predicates) {
      const message = await this.connector.waitForMessage<TPayload>(
        this.name,
        Math.max(0, deadline - Date.now()),
        (candidate) => {
          if (!predicate(candidate.payload)) {
            throw connectorError(
              ZlinkStreamErrorCode.ValidationFailed,
              `Message '${this.name}' arrived out of the expected sequence.`
            );
          }
          return true;
        },
        signal
      );
      payloads.push(message.payload);
    }
    return payloads;
  }

  private ensureConfigurable(): void {
    if (this.executed) {
      throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Builder instances can be executed only once.');
    }
  }

  private markExecuted(): void {
    this.ensureConfigurable();
    this.executed = true;
  }
}

function validateTimeout(timeoutMs: number): void {
  if (!Number.isFinite(timeoutMs) || timeoutMs < 0) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Timeout must be a non-negative finite number.');
  }
}
