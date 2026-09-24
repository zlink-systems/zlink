import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode,
  type ZlinkStreamConnector,
  type Disposable
} from '@zlink-systems/stream-connector';

export interface ChatNotify {
  actorId: string;
  name: string;
  text: string;
}

interface PingRes {
  sentAtUnixMs: string;
}

interface JoinRes {
  actorId: string;
  name: string;
}

export class EngineLobbyClient {
  private readonly connector: ZlinkStreamConnector;
  private readonly chatSubscription: Disposable;
  private readonly errorSubscription: Disposable;

  constructor(
    endpoint: string,
    private readonly onStatus: (status: string) => void,
    onChat: (notification: ChatNotify) => void
  ) {
    this.connector = zlinkStreamConnectorFactory.create({
      endpoint,
      codec: zlinkStreamJsonCodec,
      dispatchMode: ZlinkStreamDispatchMode.Manual,
      heartbeat: { enabled: false },
      reconnect: { enabled: false }
    });
    this.chatSubscription = this.connector.on<ChatNotify>('ChatNotify', (message) => {
      console.log('Engine Lobby ChatNotify:', JSON.stringify(message.payload));
      onChat(message.payload);
    });
    this.errorSubscription = this.connector.onErrorReceived((error) => {
      console.error('Engine Lobby connector error:', error);
    });
  }

  async start(name: string): Promise<JoinRes> {
    await this.connector.connect();

    const sentAtUnixMs = Date.now().toString();
    const pong = await this.connector
      .request({ sentAtUnixMs })
      .packetName('PingReq')
      .submit<PingRes>();
    if (pong.sentAtUnixMs !== sentAtUnixMs) {
      throw new Error('PingRes sentAtUnixMs mismatch.');
    }

    const joined = await this.connector.request({ name }).packetName('JoinReq').submit<JoinRes>();
    if (!joined.actorId || joined.name !== name) {
      throw new Error('JoinRes payload mismatch.');
    }
    this.onStatus(`joined as ${joined.name} (${joined.actorId})`);
    return joined;
  }

  async sendChat(text: string): Promise<void> {
    await this.connector.send({ text }).packetName('ChatMsg').submit();
  }

  async dispatch(): Promise<void> {
    await this.connector.dispatch();
  }

  async close(): Promise<void> {
    this.chatSubscription.dispose();
    this.errorSubscription.dispose();
    await this.connector.close();
  }
}
