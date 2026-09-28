import type {
  ZlinkStreamConnection,
  ZlinkStreamEncodedPayload,
  ZlinkStreamError,
  ZlinkStreamMetadata,
  ZlinkStreamMessageKind
} from '../Contracts';
import { ZlinkStreamErrorCode, ZlinkStreamException } from '../Contracts';
import type { ZlinkStreamFrameProtocol } from './Protocol/ZlinkStreamFrameProtocol';
import { connectorError, throwIfAborted } from './ZlinkStreamSupport';

interface QueuedWrite {
  readonly frame: Uint8Array;
  readonly complete: () => void;
  readonly fail: (error: unknown) => void;
}

/**
 * The frame write queue of one connection writes accepted frames in order.
 */
class ZlinkStreamConnectionWriteQueue {
  private readonly queue = new Set<QueuedWrite>();
  private active: QueuedWrite | undefined;

  /**
   * @param writeFailed Ends the connection whose transport write failed (spec
   *   stream-connector 32 §9: a transport write failure ends the connection as
   *   `TransportError`).
   */
  constructor(
    private readonly connection: ZlinkStreamConnection,
    private readonly writeFailed: (error: ZlinkStreamError) => void
  ) {}

  enqueue(operation: QueuedWrite): void {
    this.queue.add(operation);
    this.advance();
  }

  cancel(operation: QueuedWrite, error: unknown): void {
    if (this.queue.has(operation) || this.active === operation) operation.fail(error);
  }

  /**
   * Spec stream-connector 32 §7: when the connection ends, frames that have not
   * reached the transport are not written and the frame being written is not
   * waited for. The operations of both fail with `error`.
   */
  failAll(error: unknown): void {
    const operations = this.active === undefined ? [...this.queue] : [this.active, ...this.queue];
    this.queue.clear();
    this.active = undefined;
    for (const operation of operations) operation.fail(error);
  }

  private advance(): void {
    if (this.active !== undefined) return;
    const next = this.queue.values().next().value;
    if (next === undefined) return;
    this.queue.delete(next);
    this.active = next;
    let write: Promise<void>;
    try {
      // Once a frame starts writing, caller cancellation cannot interrupt it.
      write = Promise.resolve(this.connection.write(next.frame));
    } catch (error) {
      write = Promise.reject(error);
    }
    void write
      .then(
        () => next.complete(),
        (cause) => this.failWrite(next, cause)
      )
      .finally(() => {
        if (this.active !== next) return;
        this.active = undefined;
        this.advance();
      });
  }

  /**
   * Spec stream-connector 32 §9: a transport write failure is `SendFailed` for
   * the operation of that write, and it ends the connection. The ending fails
   * every other operation of the connection with `Disconnected`. A write that
   * fails after the connection already ended belongs to that ending, which has
   * already failed its operation with `Disconnected`.
   */
  private failWrite(operation: QueuedWrite, cause: unknown): void {
    if (this.active !== operation) return;
    const error: ZlinkStreamError = {
      code: ZlinkStreamErrorCode.SendFailed,
      message: `Frame write failed: ${cause instanceof Error ? cause.message : String(cause)}`,
      cause
    };
    operation.fail(new ZlinkStreamException(error));
    this.writeFailed(error);
  }
}

export class ZlinkStreamFrameSender {
  private readonly queues = new Map<ZlinkStreamConnection, ZlinkStreamConnectionWriteQueue>();

  constructor(private readonly protocol: ZlinkStreamFrameProtocol) {}

  async send(
    connection: ZlinkStreamConnection,
    kind: ZlinkStreamMessageKind,
    name: string,
    payload: ZlinkStreamEncodedPayload,
    metadata: ZlinkStreamMetadata,
    compress: boolean,
    requestSeq: bigint | undefined,
    signal?: AbortSignal,
    correlationId?: string,
    actorSlot?: number,
    expiry?: Promise<unknown>,
    onAccepted?: () => void
  ): Promise<void> {
    throwIfAborted(signal);
    await this.write(
      connection,
      this.protocol.encode(
        kind,
        name,
        payload,
        metadata,
        compress,
        requestSeq,
        correlationId,
        actorSlot
      ),
      signal,
      expiry,
      onAccepted
    );
  }

  /**
   * Writes a control frame (heartbeat ping or pong). A control frame belongs to
   * no operation: its write failure ends the connection through the write
   * queue like any other write (spec stream-connector 32 §9), and a connection
   * that has already ended does not write it. The promise settles when the
   * write has ended either way.
   */
  sendControl(connection: ZlinkStreamConnection, name: string): Promise<void> {
    const queue = this.queues.get(connection);
    if (queue === undefined) return Promise.resolve();
    const frame = this.protocol.encodeControl(name);
    return new Promise<void>((resolve) => {
      queue.enqueue({ frame, complete: resolve, fail: () => resolve() });
    });
  }

  /**
   * Gives a connection that has just been established its own write queue.
   * `writeFailed` ends that connection when one of its transport writes fails.
   */
  open(connection: ZlinkStreamConnection, writeFailed: (error: ZlinkStreamError) => void): void {
    this.queues.set(connection, new ZlinkStreamConnectionWriteQueue(connection, writeFailed));
  }

  /**
   * Ends the write queue of a connection that has ended, by close and by
   * transport loss alike. Every operation still in it fails with `error`.
   */
  failUnwritten(connection: ZlinkStreamConnection, error: unknown): void {
    const queue = this.queues.get(connection);
    if (queue === undefined) return;
    this.queues.delete(connection);
    queue.failAll(error);
  }

  private write(
    connection: ZlinkStreamConnection,
    frame: Uint8Array,
    signal?: AbortSignal,
    expiry?: Promise<unknown>,
    onAccepted?: () => void
  ): Promise<void> {
    throwIfAborted(signal);
    const queue = this.queues.get(connection);
    if (queue === undefined) {
      throw connectorError(ZlinkStreamErrorCode.Disconnected, 'Connector is not connected.');
    }
    let operation!: QueuedWrite;
    let settled = false;
    const promise = new Promise<void>((resolve, reject) => {
      const cleanup = () => signal?.removeEventListener('abort', onAbort);
      const onAbort = () =>
        queue.cancel(
          operation,
          connectorError(ZlinkStreamErrorCode.Disconnected, 'Operation canceled.')
        );
      operation = {
        frame,
        complete: () => {
          if (settled) return;
          settled = true;
          cleanup();
          resolve();
        },
        fail: (error) => {
          if (settled) return;
          settled = true;
          cleanup();
          reject(error);
        }
      };
      signal?.addEventListener('abort', onAbort, { once: true });
      onAccepted?.();
      expiry?.then(undefined, (error) => queue.cancel(operation, error));
      queue.enqueue(operation);
    });
    return promise;
  }
}
