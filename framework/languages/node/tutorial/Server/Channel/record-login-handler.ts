import { Injectable, Logger } from '@nestjs/common';
import type { ZLinkMessageContext, ZLinkSendHandler } from '@zlink-systems/framework';
import type { RecordLogin } from '../../Shared/contracts';

// --8<-- [start:channel-send-handler]
// Handles a one-way message. There is no return value, so the caller is already
// done by the time this runs and cannot observe a failure here.
@Injectable()
export class RecordLoginHandler implements ZLinkSendHandler<RecordLogin> {
  private readonly logger = new Logger(RecordLoginHandler.name);

  async handle(message: RecordLogin, context: ZLinkMessageContext): Promise<void> {
    void context;
    this.logger.log(`login recorded: ${message.playerId}`);
  }
}
// --8<-- [end:channel-send-handler]
