import { Injectable } from '@nestjs/common';
import { AgentAvailabilityDirectory } from './agent-availability-directory';

@Injectable()
class AgentAssignmentService {
  constructor(private readonly agents: AgentAvailabilityDirectory) {}

  assignNextAgent(): string | undefined {
    const actorId = this.agents.firstAvailable();
    if (actorId !== undefined) {
      this.agents.assigned(actorId);
    }
    return actorId;
  }
}

export { AgentAssignmentService };
