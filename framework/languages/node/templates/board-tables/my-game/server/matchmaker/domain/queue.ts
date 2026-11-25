import { MatchCancellation, Matchmaking } from '../../shared/constants';
import { selectPair } from './match-rule';

export interface Ticket {
  playerId: string;
  requestKey: string;
  ticketId: string;
  waitingSince: number;
  assignmentId?: string;
  disconnectedAt?: number;
}

export interface Assignment {
  id: string;
  deadline: number;
  members: [Ticket, Ticket];
  entered: string[];
  returned: string[];
  state:
    | { phase: 'preparing' }
    | { phase: 'entering' }
    | { phase: 'withdrawing'; requeue: string[] };
}

export type Decision = 'commit' | 'withdraw';

function* waitingTickets(tickets: Iterable<Ticket>): Iterable<Ticket> {
  for (const ticket of tickets) {
    if (!ticket.assignmentId && ticket.disconnectedAt === undefined) yield ticket;
  }
}
export function waitingPair(tickets: Iterable<Ticket>): [Ticket, Ticket] | undefined {
  return selectPair(waitingTickets(tickets));
}

export function waitingCancellation(ticket: Ticket, now: number): string | undefined {
  if (ticket.assignmentId) return;
  const expiresAt = ticket.waitingSince + Matchmaking.waitMs;
  const disconnectDeadline =
    ticket.disconnectedAt === undefined
      ? Infinity
      : ticket.disconnectedAt + Matchmaking.disconnectGraceMs;
  if (now >= disconnectDeadline && disconnectDeadline <= expiresAt)
    return MatchCancellation.disconnected;
  if (now >= expiresAt) return MatchCancellation.expired;
}

export function decide(assignment: Assignment, now: number, failed = false): Decision | undefined {
  if (assignment.state.phase === 'withdrawing') return undefined;
  if (now >= assignment.deadline || failed) return 'withdraw';
  if (assignment.state.phase === 'preparing') return undefined;
  if (assignment.entered.length === assignment.members.length) return 'commit';
  return undefined;
}
