import { Injectable } from '@nestjs/common';
import type {
  ZLinkMessage,
  ZLinkSession,
  ZLinkSessionContext,
  ZLinkSessionDispatchContext,
  ZLinkSessionFactory
} from '@zlink-systems/framework';
import { OpsConsoleRegistry } from './ops-console-registry';

class OpsSession implements ZLinkSession {
  constructor(readonly context: ZLinkSessionContext, private readonly consoles: OpsConsoleRegistry) {
  }

  async onConnected(): Promise<void> {
    this.consoles.add(this.context);
    console.log(`ops console connected session=${this.context.sessionId}`);
  }

  async onDisconnected(): Promise<void> {
    this.consoles.remove(this.context);
  }

  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    if (!await this.context.handlers.tryHandle(dispatch, payload)) {
      throw new Error(`Unsupported ops packet '${dispatch.packetName}'.`);
    }
  }
}

@Injectable()
class OpsSessionFactory implements ZLinkSessionFactory<OpsSession> {
  constructor(private readonly consoles: OpsConsoleRegistry) {}

  async create(context: ZLinkSessionContext): Promise<OpsSession> {
    return new OpsSession(context, this.consoles);
  }
}

export { OpsSession, OpsSessionFactory };
