import { AgentAvailabilityDirectory } from '../../../../../Application/ConversationAssignment/agent-availability-directory';
import type { ZLinkSpotTimerHandler, ZLinkTimerTick } from '@zlink-systems/framework';
import type { ConversationSpot } from '../conversation-spot';

// --8<-- [start:doc-sc-idle-timer]
class ConversationIdleTimerHandler implements ZLinkSpotTimerHandler<ConversationSpot> {
  constructor(private readonly availability: AgentAvailabilityDirectory) {}

  async handle(spot: ConversationSpot, _tick: ZLinkTimerTick): Promise<void> {
    const releasedAgent = await spot.onTimer();
    if (releasedAgent !== undefined) this.availability.released(releasedAgent);
  }
}
// --8<-- [end:doc-sc-idle-timer]

export { ConversationIdleTimerHandler };
