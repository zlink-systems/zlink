import { RunPhase, MaximumInteger, FinalStage, type PrepareDungeon, type JoinDungeon } from '../shared/messages';
import { Game, Rejection } from '../shared/constants';
export class DungeonState {
  partyId = ''; runId: string | null = null; members: string[] = []; joined: string[] = [];
  stage = 0n; phase = RunPhase.Preparing;
  prepare(request: PrepareDungeon): string | null {
    if (this.runId !== null) return this.runId === request.runId && this.partyId === request.partyId && JSON.stringify(this.members) === JSON.stringify(request.members) ? null : Rejection.runMismatch;
    if (!request.runId.trim() || !request.partyId.trim() || !request.members.length || request.members.length > Game.maximumPartyMembers || new Set(request.members).size !== request.members.length || request.members.some(id => !id.trim())) return Rejection.runMismatch;
    this.runId = request.runId; this.partyId = request.partyId; this.members = [...request.members]; this.phase = RunPhase.Entering; return null;
  }
  admit(id: string, request: JoinDungeon, dungeonId: string): boolean {
    return request.dungeonId === dungeonId && request.runId === this.runId && this.members.includes(id);
  }
  join(id: string): boolean {
    if (!this.members.includes(id)) return false;
    if (!this.joined.includes(id)) this.joined.push(id);
    if (this.phase !== RunPhase.Entering || this.joined.length !== this.members.length) return false;
    this.phase = RunPhase.Active; return true;
  }
  advance(): string | null {
    if (this.stage >= MaximumInteger || this.stage < 0n) return Rejection.integerOutOfRange;
    if (this.phase !== RunPhase.Active) return Rejection.runNotActive;
    this.stage++; if (this.stage === FinalStage) this.phase = RunPhase.Completed; return null;
  }
  capture(): string { return JSON.stringify({ ...this, stage: this.stage.toString() }); }
  static restore(payload: string): DungeonState {
    const parsed = JSON.parse(payload); return Object.assign(new DungeonState(), parsed, { stage: BigInt(parsed.stage) });
  }
}
