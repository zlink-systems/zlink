import net from 'node:net';
import type { Server, Socket } from 'node:net';

export class NetworkFaultProxy {
  private readonly sockets = new Set<Socket>();

  private constructor(
    private readonly server: Server,
    private readonly upstream: URL,
    private blocked: boolean,
    readonly endpoint: string,
    readonly redisEndpoint: string,
    readonly port: number
  ) {
    server.on('connection', (client) => this.accept(client));
  }

  static async start(
    upstreamEndpoint: string,
    initiallyBlocked = false,
    listenPort = 0
  ): Promise<NetworkFaultProxy> {
    const server = net.createServer();
    await new Promise<void>((resolve, reject) => {
      server.once('error', reject);
      server.listen(listenPort, '127.0.0.1', resolve);
    });
    const address = server.address();
    if (typeof address !== 'object' || address === null) throw new Error('Network fault proxy did not bind a TCP port.');
    const normalizedUpstream = upstreamEndpoint.includes('://')
      ? upstreamEndpoint
      : `tcp://${upstreamEndpoint}`;
    return new NetworkFaultProxy(
      server,
      new URL(normalizedUpstream.replace(/^tcp:/, 'http:')),
      initiallyBlocked,
      `tcp://127.0.0.1:${address.port}`,
      `127.0.0.1:${address.port}`,
      address.port
    );
  }

  block(): void {
    this.blocked = true;
    for (const socket of this.sockets) socket.destroy();
    this.sockets.clear();
  }

  unblock(): void {
    this.blocked = false;
  }

  async close(): Promise<void> {
    this.block();
    await new Promise<void>((resolve, reject) => this.server.close((error) => error === undefined ? resolve() : reject(error)));
  }

  private accept(client: Socket): void {
    if (this.blocked) {
      client.destroy();
      return;
    }
    const upstream = net.connect(Number(this.upstream.port), this.upstream.hostname);
    this.track(client);
    this.track(upstream);
    upstream.once('connect', () => {
      client.pipe(upstream);
      upstream.pipe(client);
    });
    upstream.once('error', () => client.destroy());
    client.once('error', () => upstream.destroy());
  }

  private track(socket: Socket): void {
    this.sockets.add(socket);
    socket.once('close', () => this.sockets.delete(socket));
  }
}
