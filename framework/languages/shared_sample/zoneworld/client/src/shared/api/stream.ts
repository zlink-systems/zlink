import { signal } from '@preact/signals';
import {
  ZlinkStreamConnectionState,
  ZlinkStreamDispatchMode,
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  type Disposable,
  type ZlinkStreamConnector,
} from '@zlink-systems/stream-connector';

export class StreamClient {
  readonly state = signal<ZlinkStreamConnectionState>(ZlinkStreamConnectionState.Created);
  readonly lastError = signal<string | null>(null);
  private readonly connector: ZlinkStreamConnector;
  private readonly registrations: Disposable[] = [];

  constructor(endpoint: string) {
    this.connector = zlinkStreamConnectorFactory.create({
      endpoint,
      codec: zlinkStreamJsonCodec,
      dispatchMode: ZlinkStreamDispatchMode.Immediate,
      heartbeat: { enabled: true },
      reconnect: { enabled: true, initialDelayMs: 100, maxDelayMs: 2_000 },
    });
    this.registrations.push(
      this.connector.onConnectionStateChanged(({ current, error }) => {
        this.state.value = current;
        if (error !== undefined) this.lastError.value = error.message;
      }),
      this.connector.onErrorReceived((error) => {
        this.lastError.value = error.message;
      }),
    );
  }

  on<T>(packetName: string, handler: (payload: T) => void): void {
    this.registrations.push(this.connector.on<T>(packetName, ({ payload }) => handler(payload), Object));
  }

  async connect(): Promise<void> {
    this.lastError.value = null;
    await this.connector.connect();
  }

  request<TReply>(packetName: string, payload: unknown): Promise<TReply> {
    return this.connector.request(payload, Object).packetName(packetName).submit<TReply>();
  }

  send(packetName: string, payload: unknown): void {
    this.connector.send(payload, Object).packetName(packetName).submit();
  }

  async close(): Promise<void> {
    for (const registration of this.registrations.splice(0)) registration.dispose();
    await this.connector.close();
  }
}
