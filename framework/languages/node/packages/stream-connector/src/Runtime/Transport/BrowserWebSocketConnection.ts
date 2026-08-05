import {
  ZlinkStreamConnection,
  ZlinkStreamErrorCode,
  ZlinkStreamTransportFactory
} from '../../Contracts';
import type { RequiredZlinkStreamConnectorOptions } from '../../Contracts';
import { connectorError, throwIfAborted } from '../ZlinkStreamSupport';

interface BrowserWebSocketEventMap {
  open: unknown;
  close: unknown;
  error: unknown;
  message: { readonly data: unknown };
}

interface BrowserWebSocket {
  binaryType: string;
  readonly readyState: number;
  send(data: Uint8Array): void;
  close(): void;
  addEventListener<TKey extends keyof BrowserWebSocketEventMap>(
    type: TKey,
    listener: (event: BrowserWebSocketEventMap[TKey]) => void,
    options?: { readonly once?: boolean }
  ): void;
  removeEventListener<TKey extends keyof BrowserWebSocketEventMap>(
    type: TKey,
    listener: (event: BrowserWebSocketEventMap[TKey]) => void
  ): void;
}

interface BrowserWebSocketConstructor {
  new(url: string): BrowserWebSocket;
}

export class BrowserStreamTransportFactory implements ZlinkStreamTransportFactory {
  async connect(
    options: RequiredZlinkStreamConnectorOptions,
    signal?: AbortSignal
  ): Promise<ZlinkStreamConnection> {
    throwIfAborted(signal);
    const WebSocketConstructor = (globalThis as { WebSocket?: BrowserWebSocketConstructor }).WebSocket;
    if (WebSocketConstructor === undefined) {
      throw connectorError(
        ZlinkStreamErrorCode.ConfigurationError,
        'The browser entrypoint requires the platform WebSocket API.'
      );
    }
    const socket = new WebSocketConstructor(options.endpoint);
    socket.binaryType = 'arraybuffer';
    await waitForOpen(socket, options.connectTimeoutMs, signal);
    return new BrowserWebSocketConnection(socket);
  }
}

export class BrowserWebSocketConnection implements ZlinkStreamConnection {
  private readonly messages: Array<Uint8Array | undefined> = [];
  private messageHead = 0;
  private closed = false;
  private error: Error | undefined;
  private readWaiter: (() => void) | undefined;

  constructor(private readonly socket: BrowserWebSocket) {
    socket.addEventListener('message', this.onMessage);
    socket.addEventListener('close', this.onClose);
    socket.addEventListener('error', this.onError);
  }

  async write(frame: Uint8Array, signal?: AbortSignal): Promise<void> {
    throwIfAborted(signal);
    if (this.closed || this.socket.readyState !== 1) {
      throw connectorError(ZlinkStreamErrorCode.Disconnected, 'Remote stream is not connected.');
    }
    try {
      this.socket.send(frame);
    } catch (cause) {
      throw connectorError(ZlinkStreamErrorCode.SendFailed, 'Send failed.', cause);
    }
  }

  async read(signal?: AbortSignal): Promise<Uint8Array | undefined> {
    throwIfAborted(signal);
    for (;;) {
      const message = this.takeMessage();
      if (message !== undefined) {
        return message;
      }
      if (this.error !== undefined) {
        throw this.error;
      }
      if (this.closed) {
        return undefined;
      }
      await this.waitForMessage(signal);
    }
  }

  async close(signal?: AbortSignal): Promise<void> {
    throwIfAborted(signal);
    if (!this.closed) {
      this.closed = true;
      this.socket.close();
      this.wakeReader();
    }
    try {
      await waitForClose(this.socket, signal);
    } finally {
      this.removeListeners();
    }
  }

  private readonly onMessage = (event: BrowserWebSocketEventMap['message']): void => {
    try {
      const message = toUint8Array(event.data);
      this.messages.push(message);
    } catch (cause) {
      this.error = cause instanceof Error
        ? cause
        : connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, 'WebSocket message decode failed.', cause);
      this.closed = true;
      this.socket.close();
    }
    this.wakeReader();
  };

  private readonly onClose = (): void => {
    if (!this.closed) {
      this.error = connectorError(ZlinkStreamErrorCode.Disconnected, 'Remote stream closed the WebSocket connection.');
    }
    this.closed = true;
    this.wakeReader();
  };

  private readonly onError = (): void => {
    this.error = connectorError(ZlinkStreamErrorCode.Disconnected, 'Remote stream closed after a WebSocket error.');
    this.closed = true;
    this.wakeReader();
  };

  private waitForMessage(signal: AbortSignal | undefined): Promise<void> {
    if (this.readWaiter !== undefined) {
      throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Only one pending stream read is supported.');
    }
    return new Promise((resolve, reject) => {
      const onAbort = () => {
        this.readWaiter = undefined;
        reject(connectorError(ZlinkStreamErrorCode.Disconnected, 'Operation canceled.'));
      };
      signal?.addEventListener('abort', onAbort, { once: true });
      this.readWaiter = () => {
        signal?.removeEventListener('abort', onAbort);
        this.readWaiter = undefined;
        resolve();
      };
      if (this.hasQueuedMessage() || this.closed) {
        this.wakeReader();
      }
    });
  }

  private wakeReader(): void {
    this.readWaiter?.();
  }

  private removeListeners(): void {
    this.socket.removeEventListener('message', this.onMessage);
    this.socket.removeEventListener('close', this.onClose);
    this.socket.removeEventListener('error', this.onError);
  }

  private hasQueuedMessage(): boolean {
    return this.messageHead < this.messages.length;
  }

  private takeMessage(): Uint8Array | undefined {
    if (!this.hasQueuedMessage()) return undefined;
    const message = this.messages[this.messageHead];
    this.messages[this.messageHead] = undefined;
    this.messageHead += 1;
    if (this.messageHead >= 1024 && this.messageHead * 2 >= this.messages.length) {
      this.messages.splice(0, this.messageHead);
      this.messageHead = 0;
    }
    return message;
  }
}

function waitForClose(socket: BrowserWebSocket, signal: AbortSignal | undefined): Promise<void> {
  if (socket.readyState === 3) return Promise.resolve();
  return new Promise((resolve, reject) => {
    const onClose = () => finish();
    const onAbort = () => finish(connectorError(ZlinkStreamErrorCode.Disconnected, 'Close canceled.'));
    const finish = (error?: Error) => {
      socket.removeEventListener('close', onClose);
      signal?.removeEventListener('abort', onAbort);
      if (error === undefined) resolve();
      else reject(error);
    };
    socket.addEventListener('close', onClose, { once: true });
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

async function waitForOpen(
  socket: BrowserWebSocket,
  connectTimeoutMs: number,
  signal: AbortSignal | undefined
): Promise<void> {
  throwIfAborted(signal);
  await new Promise<void>((resolve, reject) => {
    const timeout = setTimeout(() => finish(
      connectorError(ZlinkStreamErrorCode.ConnectTimeout, 'Connect timed out.')
    ), connectTimeoutMs);
    const onOpen = () => finish();
    const onClose = () => finish(connectorError(ZlinkStreamErrorCode.ConnectTimeout, 'Connect closed before opening.'));
    const onError = () => finish(connectorError(ZlinkStreamErrorCode.ConnectTimeout, 'Connect failed.'));
    const onAbort = () => finish(connectorError(ZlinkStreamErrorCode.Disconnected, 'Connect canceled.'));
    const finish = (error?: Error) => {
      clearTimeout(timeout);
      socket.removeEventListener('open', onOpen);
      socket.removeEventListener('close', onClose);
      socket.removeEventListener('error', onError);
      signal?.removeEventListener('abort', onAbort);
      if (error === undefined) {
        resolve();
      } else {
        socket.close();
        reject(error);
      }
    };
    socket.addEventListener('open', onOpen, { once: true });
    socket.addEventListener('close', onClose, { once: true });
    socket.addEventListener('error', onError, { once: true });
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

function toUint8Array(data: unknown): Uint8Array {
  if (data instanceof ArrayBuffer) {
    return new Uint8Array(data);
  }
  if (ArrayBuffer.isView(data)) {
    return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  }
  throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, 'WebSocket text messages are not supported.');
}
