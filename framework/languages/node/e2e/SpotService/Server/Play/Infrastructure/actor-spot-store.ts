export class InMemoryActorSpotStore {
  private static readonly spotByActor = new Map<string, string>();

  static record(actorId: string, spotId: string): void {
    this.spotByActor.set(actorId, spotId);
  }

  static find(actorId: string): string | undefined {
    return this.spotByActor.get(actorId);
  }
}
