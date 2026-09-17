import { Injectable, Logger } from '@nestjs/common';
import type { ZLinkFanoutHandler, ZLinkPublishMessageContext } from '@zlink-systems/framework';
import type { MaintenanceNotice } from '../../Shared/contracts';

// --8<-- [start:fanout-handler]
// Receives what any publisher on this channel sends. The publisher does not know
// this node exists, so adding or removing a subscriber changes nothing there.
@Injectable()
export class MaintenanceNoticeSubscriber implements ZLinkFanoutHandler<MaintenanceNotice> {
  private readonly logger = new Logger(MaintenanceNoticeSubscriber.name);

  async handle(message: MaintenanceNotice, context: ZLinkPublishMessageContext): Promise<void> {
    void context;
    this.logger.log(`maintenance notice: ${message.message}`);
  }
}
// --8<-- [end:fanout-handler]
