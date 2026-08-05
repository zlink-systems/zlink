import { Injectable } from '@nestjs/common';
import { Conversation } from '../../Domain/SupportChat/conversation';

@Injectable()
class SupportConversationAllocator {
  private nextId = 1;
  allocate(customerActorId: string, customerDisplayName: string, subject: string): Conversation {
    return new Conversation(`support-${this.nextId++}`, customerActorId, customerDisplayName, subject);
  }
}

export { SupportConversationAllocator };
