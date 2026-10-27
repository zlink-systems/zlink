import { Game, dungeonId } from '../shared/constants';
import { ActionRejected, JoinDungeon, PrepareDungeon, type RunPhase } from '../shared/messages';
export class DungeonState {
  partyId = ''; runId = ''; members: string[] = []; entered: string[] = [];
  stage = 0; phase: RunPhase = 'Preparing';
  prepare(request: PrepareDungeon, target: string): ActionRejected | null {
    if (dungeonId(request.runId) !== target) return new ActionRejected('run-mismatch');
    if (this.runId) return this.runId === request.runId && this.partyId === request.partyId
      && JSON.stringify(this.members) === JSON.stringify(request.members) ? null : new ActionRejected('run-mismatch');
    if (!request.members.length || request.members.length > Game.maximumPartyMembers
      || new Set(request.members).size !== request.members.length || request.members.some(id => !id.trim()))
      return new ActionRejected('invalid-party');
    this.runId = request.runId; this.partyId = request.partyId;
    this.members = [...request.members]; this.phase = 'Entering'; return null;
  }
  admit(id: string, join: JoinDungeon, target: string): boolean {
    return join.dungeonId === target && join.runId === this.runId && this.members.includes(id);
  }
  arrived(id: string): void {
    if (!this.entered.includes(id)) this.entered.push(id);
    if (this.entered.length === this.members.length && this.phase === 'Entering') this.phase = 'Active';
  }
  advance(): ActionRejected | null {
    if (this.phase !== 'Active') return new ActionRejected('run-not-active');
    if (++this.stage === Game.finalStage) this.phase = 'Completed';
    return null;
  }
}
