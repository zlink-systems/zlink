import { Game, Rejection } from '../shared/constants';
import { PartyView, RunPhase, newRunId, type PartyJoin } from '../shared/messages';
export class PartyState {
  partyId = '';
  phase = RunPhase.Forming;
  runId: string | null = null;
  entered: string[] = [];
  members: string[] = [];
  ready: string[] = [];
  join(id: string, request: PartyJoin): boolean {
    if (
      this.phase !== RunPhase.Forming ||
      !request.partyId.trim() ||
      this.members.includes(id) ||
      this.members.length >= Game.maximumPartyMembers ||
      (request.create
        ? this.members.length !== 0
        : this.partyId !== request.partyId || this.members.length === 0)
    )
      return false;
    this.partyId = request.partyId;
    this.members.push(id);
    return true;
  }
  markReady(id: string): void {
    if (this.phase === RunPhase.Forming && this.members.includes(id) && !this.ready.includes(id)) this.ready.push(id);
  }
  leave(id: string): void {
    if (this.phase !== RunPhase.Forming) return;
    this.members = this.members.filter((m) => m !== id);
    this.ready = this.ready.filter((m) => m !== id);
  }
  view(): PartyView {
    return new PartyView(
      this.partyId,
      [...this.members],
      this.members.map((id) => this.ready.includes(id)), this.phase, this.runId
    );
  }
  entryRejection(id: string): string | null {
    return this.members[0] !== id
      ? Rejection.notLeader
      : this.members.some((m) => !this.ready.includes(m))
        ? Rejection.notReady
        : null;
  }
  enter(id: string): string | null {
    const reason = this.entryRejection(id); if (reason) return reason;
    if (this.phase === RunPhase.Forming || this.phase === RunPhase.Completed) {
      this.runId = newRunId(); this.phase = RunPhase.Preparing; this.entered = [];
    }
    return null;
  }
  arrived(runId: string, playerId: string): string | null {
    if (this.runId !== runId || this.phase === RunPhase.Forming || !this.members.includes(playerId)) return Rejection.runMismatch;
    if (!this.entered.includes(playerId)) this.entered.push(playerId); return null;
  }
  prepared(id: string): string | null {
    if (this.runId !== id || ![RunPhase.Preparing, RunPhase.Entering].includes(this.phase)) return Rejection.runMismatch;
    this.phase = RunPhase.Entering; return null;
  }
  started(id: string): string | null {
    if (this.runId !== id || ![RunPhase.Entering, RunPhase.Active].includes(this.phase)) return Rejection.runMismatch;
    this.phase = RunPhase.Active; return null;
  }
  completed(id: string): string | null {
    if (this.runId !== id || ![RunPhase.Active, RunPhase.Completed].includes(this.phase)) return Rejection.runMismatch;
    this.phase = RunPhase.Completed; return null;
  }

}
