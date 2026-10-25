import { Game, Rejection, dungeonId } from '../shared/constants';
import { JoinDungeon, PrepareDungeon, RunPhase } from '../shared/messages';

export interface DungeonSnapshot {
  partyId: string;
  runId: string;
  ownerSpotId: string;
  members: string[];
  joined: string[];
  phase: RunPhase;
  stage: number;
}

export class DungeonState {
  partyId = '';
  runId = '';
  ownerSpotId = '';
  members: string[] = [];
  joined = new Set<string>();
  phase = RunPhase.Forming;
  stage = 0n;

  prepare(targetDungeonId: string, request: PrepareDungeon): string | null {
    if (this.runId.length !== 0)
      return this.runId === request.runId &&
        this.partyId === request.partyId &&
        this.ownerSpotId === request.ownerSpotId &&
        this.members.length === request.members.length &&
        this.members.every((member, index) => member === request.members[index])
        ? null
        : Rejection.runMismatch;
    if (
      !DungeonState.validId(request.runId, 'run-') ||
      !DungeonState.validId(request.partyId, 'party-') ||
      !request.ownerSpotId.trim() ||
      dungeonId(request.runId) !== targetDungeonId ||
      request.members.length < 1 ||
      request.members.length > Game.maximumPartyMembers ||
      new Set(request.members).size !== request.members.length ||
      request.members.some((id) => !id.trim())
    )
      return Rejection.runMismatch;
    this.runId = request.runId;
    this.partyId = request.partyId;
    this.ownerSpotId = request.ownerSpotId;
    this.members = [...request.members];
    this.phase = RunPhase.Entering;
    return null;
  }

  static validId(value: string, prefix: string): boolean {
    if (!value.startsWith(prefix) || value.length !== prefix.length + 32) return false;
    return [...value.slice(prefix.length)].every(
      (character) =>
        (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')
    );
  }

  accepts(actorId: string, targetDungeonId: string, request: JoinDungeon): boolean {
    return (
      request.dungeonId === targetDungeonId &&
      request.runId === this.runId &&
      this.members.includes(actorId)
    );
  }

  join(actorId: string): boolean {
    if (!this.members.includes(actorId)) return false;
    this.joined.add(actorId);
    if (this.phase !== RunPhase.Entering || this.joined.size !== this.members.length) return false;
    this.phase = RunPhase.Active;
    return true;
  }

  advance(): string | null {
    if (this.stage < 0n || this.stage >= Game.maximumInteger)
      return Rejection.integerOutOfRange;
    if (this.phase !== RunPhase.Active) return Rejection.runNotActive;
    this.stage++;
    if (this.stage === Game.finalStage) this.phase = RunPhase.Completed;
    return null;
  }

  capture(): DungeonSnapshot {
    return {
      partyId: this.partyId,
      runId: this.runId,
      ownerSpotId: this.ownerSpotId,
      members: [...this.members],
      joined: [...this.joined],
      phase: this.phase,
      stage: Number(this.stage)
    };
  }

  restore(snapshot: DungeonSnapshot): void {
    this.partyId = snapshot.partyId;
    this.runId = snapshot.runId;
    this.ownerSpotId = snapshot.ownerSpotId;
    this.members = [...snapshot.members];
    this.joined = new Set(snapshot.joined);
    this.phase = snapshot.phase;
    this.stage = BigInt(snapshot.stage);
  }
}
