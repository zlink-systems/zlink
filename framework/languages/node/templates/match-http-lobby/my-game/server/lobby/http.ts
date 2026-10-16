import { Inject, Injectable } from '@nestjs/common';
import { createServer, type Server, type ServerResponse } from 'node:http';
import { once } from 'node:events';
import { ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import { ZLinkSpotCreateState, type ZLinkSpotManager } from '@zlink-systems/framework';
import { NODE_CONFIG, type NodeConfig } from '../shared/config';
import { Names, Http } from '../shared/constants';
import { MatchRequest, MatchResponse } from '../shared/messages';
import { Matchmaker, InvalidMatchRequest } from './matchmaker';
@Injectable()
export class LobbyHttp {
  private server?: Server;
  private readonly shutdown = new AbortController();
  private readonly active = new Set<Promise<void>>();
  endpoint = '';
  constructor(
    @Inject(ZLINK_SPOT_MANAGER) spots: ZLinkSpotManager,
    @Inject(NODE_CONFIG) private readonly config: NodeConfig
  ) {
    this.matches = new Matchmaker(async (roomId) => {
      const created = await spots.getOrCreate(roomId, Names.room).inMesh(Names.mesh).submit();
      if (created.state === ZLinkSpotCreateState.Rejected)
        throw new Error('Room creation rejected');
      if (!config.sessionEndpoint) throw new Error('Session endpoint required');
      return new MatchResponse(String(created.spot.spotId), config.sessionEndpoint);
    });
  }
  private readonly matches: Matchmaker;
  async start(): Promise<void> {
    if (!this.config.httpEndpoint || !this.config.sessionEndpoint)
      throw new Error('HTTP and Session endpoints required');
    const address = new URL(this.config.httpEndpoint);
    if (address.protocol !== Http.protocol) throw new Error('HTTP endpoint required');
    this.server = createServer((request, response) => {
      const task = this.handle(request, response);
      this.active.add(task);
      void task.finally(() => this.active.delete(task));
    });
    this.server.listen(Number(address.port), address.hostname);
    await once(this.server, 'listening');
    const bound = this.server.address();
    if (!bound || typeof bound === 'string') throw new Error('HTTP listener address missing');
    this.endpoint = `http://${address.hostname}:${bound.port}`;
  }
  private async handle(
    request: import('node:http').IncomingMessage,
    response: ServerResponse
  ): Promise<void> {
    const disconnected = new AbortController();
    const onClose = (): void => {
      if (!response.writableFinished) disconnected.abort(new Error('HTTP client disconnected'));
    };
    response.on('close', onClose);
    try {
      if (request.method !== Http.post || request.url !== Names.matchPath) {
        this.reply(response, Http.notFound, { error: 'Unknown endpoint' });
        return;
      }
      const chunks: Buffer[] = [];
      for await (const chunk of request) chunks.push(chunk as Buffer);
      const value = JSON.parse(Buffer.concat(chunks).toString('utf8')) as MatchRequest | null;
      if (!value || typeof value !== 'object' || Array.isArray(value))
        throw new InvalidMatchRequest('Match request object required');
      const assignment = await this.matches.match(
        new MatchRequest(value.playerId, value.mode),
        AbortSignal.any([disconnected.signal, this.shutdown.signal])
      );
      this.reply(response, Http.ok, assignment);
    } catch (error) {
      console.error('HTTP match failed', error);
      this.reply(
        response,
        error instanceof InvalidMatchRequest || error instanceof SyntaxError
          ? Http.badRequest
          : Http.unavailable,
        { error: error instanceof Error ? error.message : String(error) }
      );
    } finally {
      response.off('close', onClose);
    }
  }
  private reply(response: ServerResponse, status: number, value: object): void {
    if (response.destroyed) return;
    response.writeHead(status, { 'content-type': Http.contentType });
    response.end(JSON.stringify(value));
  }
  async close(): Promise<void> {
    this.shutdown.abort(new Error('Lobby shutting down'));
    if (this.server) {
      const closed = new Promise<void>((resolve, reject) =>
        this.server!.close((error) => (error ? reject(error) : resolve()))
      );
      await Promise.all(this.active);
      this.server.closeIdleConnections();
      await closed;
    }
  }
}
