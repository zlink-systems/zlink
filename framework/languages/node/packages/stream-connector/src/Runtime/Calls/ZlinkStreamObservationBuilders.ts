import {
  ZlinkStreamErrorCode,
  type RequiredZlinkStreamConnectorOptions,
  type ZlinkStreamExpectNoneCall,
  type ZlinkStreamMessage,
  type ZlinkStreamSequenceCall
} from '../../Contracts';
import { connectorError } from '../ZlinkStreamSupport';

export interface ZlinkStreamMessageWaiter {
  readonly options: RequiredZlinkStreamConnectorOptions;
  waitForMessage<TPayload>(
    name: string,
    timeoutMs: number,
    predicate: (message: ZlinkStreamMessage<TPayload>) => boolean,
    signal?: AbortSignal
  ): Promise<ZlinkStreamMessage<TPayload> | undefined>;
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
    // Spec stream-connector 32 §10.1.1: the window elapsing is this surface's
    // success; a message inside it is the violated observation. A connection
    // that ended rejects the wait itself, as `Disconnected`.
    const message = await this.connector.waitForMessage<TPayload>(this.name, this.windowMs, () => true, signal);
    if (message === undefined) {
      return;
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
  private readonly predicates: Array<(message: ZlinkStreamMessage<TPayload>) => boolean> = [];
  private timeoutMs: number | undefined;
  private executed = false;

  constructor(
    private readonly connector: ZlinkStreamMessageWaiter,
    private readonly name: string
  ) {}

  expect(predicate: (message: ZlinkStreamMessage<TPayload>) => boolean): this {
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

  // Spec stream-connector 32 §10.1: the predicate reads the whole message, and
  // the call answers with the messages themselves, so a caller can assert on the
  // packet name and the metadata and not only on the payload.
  async run(signal?: AbortSignal): Promise<readonly ZlinkStreamMessage<TPayload>[]> {
    this.markExecuted();
    if (this.predicates.length === 0) {
      throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'waitForSequence requires at least one expectation.');
    }
    const timeoutMs = this.timeoutMs ?? this.connector.options.waitTimeoutMs;
    const deadline = Date.now() + timeoutMs;
    const messages: ZlinkStreamMessage<TPayload>[] = [];
    for (const predicate of this.predicates) {
      const message = await this.connector.waitForMessage<TPayload>(
        this.name,
        Math.max(0, deadline - Date.now()),
        (candidate) => {
          if (!predicate(candidate)) {
            throw connectorError(
              ZlinkStreamErrorCode.ValidationFailed,
              `Message '${this.name}' arrived out of the expected sequence.`
            );
          }
          return true;
        },
        signal
      );
      if (message === undefined) {
        // Spec stream-connector 32 §10.1.1: the sequence did not complete
        // inside the window, a violated observation.
        throw connectorError(
          ZlinkStreamErrorCode.ValidationFailed,
          `The '${this.name}' sequence did not complete within ${timeoutMs}ms.`
        );
      }
      messages.push(message);
    }
    return messages;
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
