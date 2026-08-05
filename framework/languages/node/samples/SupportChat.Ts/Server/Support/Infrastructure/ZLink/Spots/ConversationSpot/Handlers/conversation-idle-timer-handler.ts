import { AgentAvailabilityDirectory } from '../../../../../Application/ConversationAssignment/agent-availability-directory';
import type { ZLinkSpotTimerHandler, ZLinkTimerTick } from '@zlink-systems/framework';
import type { ConversationSpot } from '../conversation-spot';

class ConversationIdleTimerHandler implements ZLinkSpotTimerHandler<ConversationSpot> {
  constructor(private readonly availability: AgentAvailabilityDirectory) {}

  async handle(spot: ConversationSpot, tick: ZLinkTimerTick): Promise<void> {
    const releasedAgent = await spot.onTimer(tick.scheduledAt.getTime());
    if (releasedAgent !== undefined) this.availability.released(releasedAgent);
  }
}

export { ConversationIdleTimerHandler };
