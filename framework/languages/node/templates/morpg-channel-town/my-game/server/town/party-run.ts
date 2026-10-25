import { randomUUID } from 'node:crypto';
import { Rejection } from '../shared/constants';
import { PartyView, RunPhase } from '../shared/messages';

export interface PartyRunSnapshot {
  partyId: string;
  members: string[];
  phase: RunPhase;
  runId: string | null;
}

function newId(prefix: string): string {
  return prefix + randomUUID().replaceAll('-', '');
}

export class PartyRun {
  constructor(
    readonly partyId = newId('party-'),
    readonly members: string[] = [],
    public phase: RunPhase = RunPhase.Forming,
    public runId: string | null = null
  ) {}

  enter(playerId: string): string | null {
    if (this.members[0] !== playerId) return Rejection.notLeader;
    if (this.phase === RunPhase.Forming || this.phase === RunPhase.Completed) {
      this.runId = newId('run-');
      this.phase = RunPhase.Preparing;
    }
    return null;
  }

  prepared(runId: string): string | null {
    if (
      this.runId !== runId ||
      (this.phase !== RunPhase.Preparing && this.phase !== RunPhase.Entering)
    )
      return Rejection.runMismatch;
    this.phase = RunPhase.Entering;
    return null;
  }

  started(runId: string): string | null {
    if (this.runId !== runId || (this.phase !== RunPhase.Entering && this.phase !== RunPhase.Active))
      return Rejection.runMismatch;
    this.phase = RunPhase.Active;
    return null;
  }

  completed(runId: string): string | null {
    if (this.runId !== runId || (this.phase !== RunPhase.Active && this.phase !== RunPhase.Completed))
      return Rejection.runMismatch;
    this.phase = RunPhase.Completed;
    return null;
  }

  leave(playerId: string): void {
    if (this.phase === RunPhase.Forming) {
      const index = this.members.indexOf(playerId);
      if (index >= 0) this.members.splice(index, 1);
    }
  }

  changeChannel(): string | null {
    return this.phase === RunPhase.Forming ? null : Rejection.runInProgress;
  }

  view(): PartyView {
    return new PartyView(this.partyId, [...this.members], this.phase, this.runId);
  }

  capture(): PartyRunSnapshot {
    return {
      partyId: this.partyId,
      members: [...this.members],
      phase: this.phase,
      runId: this.runId
    };
  }

  static restore(snapshot: PartyRunSnapshot): PartyRun {
    return new PartyRun(
      snapshot.partyId,
      [...snapshot.members],
      snapshot.phase,
      snapshot.runId
    );
  }
}
