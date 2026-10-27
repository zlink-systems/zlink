import { randomUUID } from 'node:crypto';
import { Game } from '../shared/constants';
import { ActionRejected, PartyView, type RunPhase } from '../shared/messages';
export const newId = (prefix: string): string => prefix + randomUUID().replaceAll('-', '');
export const validId = (id: string, prefix: string): boolean => new RegExp(`^${prefix}[0-9a-f]{32}$`).test(id);
export class PartyRun {
  members: string[] = [];
  entered: string[] = [];
  phase: RunPhase = 'Forming';
  runId: string | null = null;
  join(id: string): ActionRejected | null {
    if (this.phase !== 'Forming' || this.members.includes(id)
      || this.members.length >= Game.maximumPartyMembers || !id.trim()) return new ActionRejected('invalid-party');
    this.members.push(id); return null;
  }
  leave(id: string): void { if (this.phase === 'Forming') this.members = this.members.filter(m => m !== id); }
  enter(id: string): ActionRejected | null {
    if (this.members[0] !== id) return new ActionRejected('not-leader');
    if (this.phase === 'Forming' || this.phase === 'Completed') {
      this.runId = newId('run-'); this.entered = []; this.phase = 'Preparing';
    }
    return null;
  }
  prepared(run: string): ActionRejected | null {
    if (this.runId !== run || !['Preparing', 'Entering'].includes(this.phase)) return new ActionRejected('run-mismatch');
    this.phase = 'Entering'; return null;
  }
  arrived(run: string, id: string): ActionRejected | null {
    if (this.runId !== run || !this.members.includes(id) || !['Entering', 'Active'].includes(this.phase))
      return new ActionRejected('run-mismatch');
    if (!this.entered.includes(id)) this.entered.push(id);
    return null;
  }
  started(run: string): ActionRejected | null {
    if (this.runId !== run || !['Entering', 'Active'].includes(this.phase)
      || this.entered.length !== this.members.length) return new ActionRejected('run-mismatch');
    this.phase = 'Active'; return null;
  }
  completed(run: string): ActionRejected | null {
    if (this.runId !== run || !['Active', 'Completed'].includes(this.phase)) return new ActionRejected('run-mismatch');
    this.phase = 'Completed'; return null;
  }
  view(id: string): PartyView { return new PartyView(id, [...this.members], this.members[0] ?? '', this.phase, this.runId); }
}
