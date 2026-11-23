import { Injectable } from '@nestjs/common';
import type {
  ZLinkSession,
  ZLinkSessionContext,
  ZLinkSessionFactory,
  ZLinkStreamError
} from '@zlink-systems/framework';
export class Session implements ZLinkSession {
  constructor(readonly context: ZLinkSessionContext) {}
  async onConnected(): Promise<void> {
    console.log('Game client connected to the Session node');
  }
  async onDisconnected(): Promise<void> {
    console.log('Game client disconnected from the Session node');
  }
  async onError(_context: ZLinkSessionContext, error: ZLinkStreamError): Promise<void> {
    console.error('Session error', error);
  }
  async onDispatch(): Promise<void> {
    throw new Error('No application packets are registered yet.');
  }
}
@Injectable()
export class SessionFactory implements ZLinkSessionFactory<Session> {
  async create(context: ZLinkSessionContext): Promise<Session> {
    return new Session(context);
  }
}
