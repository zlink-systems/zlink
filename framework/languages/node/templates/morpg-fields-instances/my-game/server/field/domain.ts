import { Game, Rejection, fieldId } from '../shared/constants';
import { EnterField, PartyView } from '../shared/messages';
export class FieldState {
  visible: Record<string, EnterField> = Object.create(null);
  arrivals: Record<string, EnterField> = Object.create(null);
  parties: Record<string, string[]> = Object.create(null);
  restore(snapshot: FieldState): void {
    this.visible = Object.assign(Object.create(null), snapshot.visible);
    this.arrivals = Object.assign(Object.create(null), snapshot.arrivals);
    this.parties = Object.assign(Object.create(null), snapshot.parties);
  }
  admit(id: string, position: EnterField, target: string): boolean {
    if (fieldId(position.x) !== target) return false;
    if (this.arrivals[id]) throw new Error('Duplicate field admission');
    this.arrivals[id] = position;
    return true;
  }
  arrive(id: string): EnterField {
    const position = this.arrivals[id];
    if (!position) throw new Error('Field admission has no movement intent');
    delete this.arrivals[id];
    this.visible[id] = position;
    return position;
  }
  leave(id: string): void {
    delete this.visible[id];
    for (const [party, members] of Object.entries(this.parties)) {
      const index = members.indexOf(id);
      if (index < 0) continue;
      members.splice(index, 1);
      if (!members.length) delete this.parties[party];
      break;
    }
  }
  party(id: string, party: string, create: boolean): string | undefined {
    const members = this.parties[party];
    if (
      !party.trim() ||
      !this.visible[id] ||
      Object.values(this.parties).some((m) => m.includes(id)) ||
      (create ? !!members : !members || members.length >= Game.maximumPartyMembers)
    )
      return Rejection.invalidParty;
    if (create) this.parties[party] = [id];
    else members.push(id);
    return undefined;
  }
  ledBy(id: string): [string, string[]] | undefined {
    return Object.entries(this.parties).find(([, members]) => members[0] === id);
  }
  partyViews(): PartyView[] {
    return Object.entries(this.parties)
      .sort(([a], [b]) => a.localeCompare(b))
      .map(([id, members]) => new PartyView(id, [...members]));
  }
}
