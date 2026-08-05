import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT } from '@zlink-systems/nestjs';
import { DeliverSupportNotification } from '../../../Actors/support-user-actor';
import { ConversationEventMapper } from './conversation-event-mapper';
import type { ConversationEvent } from '../../../../../Domain/SupportChat/conversation-events';
import type { ZLinkActorClient } from '@zlink-systems/framework';

@Injectable()
class SupportNotificationPublisher {
  private readonly mapper = new ConversationEventMapper();

  constructor(@Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient) {}

  async publish(event: ConversationEvent, recipients: Iterable<string>): Promise<void> {
    const message = this.mapper.map(event);
    for (const actorId of recipients) {
      await this.actors
        .sendToActor(
          actorId,
          new DeliverSupportNotification(message, event.state.conversationId)
        )
        .submit();
    }
  }
}

export { SupportNotificationPublisher };
