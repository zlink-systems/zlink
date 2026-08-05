class AgentAvailabilityDirectory {
  private readonly available = new Set<string>();
  private readonly assignmentCounts = new Map<string, number>();
  private readonly capacity = 3;

  setAvailable(agentActorId: string, isAvailable: boolean): boolean {
    if (isAvailable) {
      this.available.add(agentActorId);
    } else {
      this.available.delete(agentActorId);
    }
    return this.available.has(agentActorId);
  }

  firstAvailable(): string | undefined {
    return [...this.available]
      .filter((actorId) => (this.assignmentCounts.get(actorId) ?? 0) < this.capacity)
      .sort((left, right) => (this.assignmentCounts.get(left) ?? 0) - (this.assignmentCounts.get(right) ?? 0))[0];
  }

  assigned(actorId: string): void {
    this.assignmentCounts.set(actorId, (this.assignmentCounts.get(actorId) ?? 0) + 1);
  }

  released(actorId: string): void {
    this.assignmentCounts.set(actorId, Math.max(0, (this.assignmentCounts.get(actorId) ?? 0) - 1));
  }
}

export { AgentAvailabilityDirectory };
