import { randomUUID } from 'node:crypto';
import { performance } from 'node:perf_hooks';
import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT } from '@zlink-systems/nestjs';
import {
  type ZLinkActorClient,
  type ZLinkInstanceSpot,
  type ZLinkInstanceSpotContext,
  type ZLinkSpotRelocationAdapter,
  type ZLinkSpotTimerHandler
} from '@zlink-systems/framework';
import { MatchCancellation, Matchmaking, Names, Reasons } from '../shared/constants';
import {
  CommitRoom,
  MatchAssigned,
  MatchCancelled,
  MatchQueued,
  MatchReleased,
  PrepareRoom,
  type RoomClosed,
  type RoomPrepared,
  WithdrawRoom,
  type CancelTicket,
  type EnterReport,
  type MatchPresence,
  type MatchRequest,
  type ReturnReport,
  type RoomWithdrawn
} from '../shared/messages';
import {
  decide,
  waitingCancellation,
  waitingPair,
  type Assignment,
  type Ticket
} from './domain/queue';

@Injectable({ scope: Scope.TRANSIENT })
export class Pool implements ZLinkInstanceSpot {
  readonly context!: ZLinkInstanceSpotContext;
  readonly tickets = new Map<string, Ticket>();
  readonly assignments = new Map<string, Assignment>();

  constructor(@Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient) {}

  configure(): void {
    this.context.handlers.addTimerHandler(PoolTick);
  }

  async onInitialize(): Promise<void> {
    await this.context.addTimer('matching', Matchmaking.tickMs, PoolTick);
  }

  async enqueue(request: MatchRequest): Promise<MatchQueued> {
    let ticket = this.tickets.get(request.playerId);
    if (ticket && ticket.requestKey !== request.requestKey)
      throw new Error('Player already queued');
    if (!ticket) {
      ticket = {
        playerId: request.playerId,
        requestKey: request.requestKey,
        ticketId: randomUUID(),
        waitingSince: performance.now()
      };
      this.tickets.set(ticket.playerId, ticket);
    }
    await this.advance();
    return new MatchQueued(ticket.ticketId);
  }

  cancel(request: CancelTicket): MatchCancelled {
    for (const ticket of this.tickets.values()) {
      if (ticket.ticketId !== request.ticketId || ticket.assignmentId) continue;
      this.tickets.delete(ticket.playerId);
      return new MatchCancelled(ticket.ticketId, MatchCancellation.cancelled);
    }
    return new MatchCancelled(request.ticketId, Reasons.matchNotWaiting);
  }

  async presence(message: MatchPresence): Promise<void> {
    for (const ticket of this.tickets.values()) {
      if (ticket.ticketId !== message.ticketId) continue;
      if (message.connected) {
        const reason = waitingCancellation(ticket, performance.now());
        if (reason) await this.expire(ticket, reason);
        else delete ticket.disconnectedAt;
      } else {
        ticket.disconnectedAt ??= performance.now();
      }
      return;
    }
  }

  private async expire(ticket: Ticket, reason: string): Promise<void> {
    this.tickets.delete(ticket.playerId);
    await this.actors
      .sendToActor(ticket.playerId, new MatchCancelled(ticket.ticketId, reason))
      .submit();
  }

  async advance(): Promise<void> {
    const now = performance.now();
    for (const assignment of this.assignments.values()) {
      if (decide(assignment, now) !== 'withdraw') continue;
      if (assignment.state.phase === 'preparing') this.release(assignment);
      else await this.withdraw(assignment);
    }
    for (const ticket of this.tickets.values()) {
      const reason = waitingCancellation(ticket, now);
      if (reason) await this.expire(ticket, reason);
    }
    const members = waitingPair(this.tickets.values());
    if (members) await this.prepare(members, performance.now());
  }

  private async prepare(members: [Ticket, Ticket], now: number): Promise<void> {
    const id = Names.matchRoomPrefix + randomUUID();
    const assignment: Assignment = {
      id,
      deadline: now + Matchmaking.entryMs,
      members,
      entered: [],
      returned: [],
      state: { phase: 'preparing' }
    };
    for (const ticket of members) ticket.assignmentId = id;
    this.assignments.set(id, assignment);
    try {
      await this.context.outbound
        .sendToSpot(
          Names.roomProvisionerKey,
          new PrepareRoom(
            id,
            members.map((ticket) => ticket.playerId)
          )
        )
        .instanceSpot(Names.roomProvisioner)
        .inMesh(Names.mesh)
        .submit();
    } catch (error) {
      // The reservation still follows its entry deadline.
      console.error('Room preparation send failed', id, error);
    }
  }

  async roomPrepared(message: RoomPrepared): Promise<void> {
    const assignment = this.assignments.get(message.assignmentId);
    if (assignment && assignment.state.phase !== 'preparing') return;
    if (!assignment || !message.ok || decide(assignment, performance.now()) === 'withdraw') {
      if (assignment) this.release(assignment);
      await this.closeRoom(message.assignmentId);
      return;
    }
    assignment.state = { phase: 'entering' };
    for (const ticket of assignment.members) {
      await this.actors
        .sendToActor(
          ticket.playerId,
          new MatchAssigned(ticket.ticketId, assignment.id, assignment.id)
        )
        .submit();
    }
  }

  private release(assignment: Assignment): void {
    this.assignments.delete(assignment.id);
    for (const ticket of assignment.members) {
      delete ticket.assignmentId;
      if (assignment.state.phase !== 'preparing') this.tickets.delete(ticket.playerId);
    }
  }

  async report(report: EnterReport): Promise<void> {
    const assignment = this.assignments.get(report.assignmentId);
    const ticket = assignment?.members.find((member) => member.ticketId === report.ticketId);
    if (
      !assignment ||
      !ticket ||
      assignment.state.phase !== 'entering' ||
      assignment.entered.includes(ticket.playerId)
    )
      return;
    const now = performance.now();
    if (report.ok && now < assignment.deadline) assignment.entered.push(ticket.playerId);
    const decision = decide(assignment, now, !report.ok);
    if (decision === 'withdraw') {
      await this.withdraw(assignment, now < assignment.deadline ? ticket.playerId : undefined);
    }
    if (decision === 'commit') {
      await this.context.outbound.sendToSpot(assignment.id, new CommitRoom(assignment.id)).submit();
      this.release(assignment);
    }
  }

  private async withdraw(assignment: Assignment, failedPlayer?: string): Promise<void> {
    const requeue =
      failedPlayer === undefined
        ? assignment.entered
        : assignment.members
            .filter((ticket) => ticket.playerId !== failedPlayer)
            .map((ticket) => ticket.playerId);
    assignment.state = { phase: 'withdrawing', requeue };
    await this.context.outbound.sendToSpot(assignment.id, new WithdrawRoom(assignment.id)).submit();
  }

  async roomWithdrawn(message: RoomWithdrawn): Promise<void> {
    const assignment = this.assignments.get(message.assignmentId);
    if (assignment?.state.phase !== 'withdrawing') return;
    for (const ticket of assignment.members)
      await this.actors
        .sendToActor(
          ticket.playerId,
          new MatchReleased(
            ticket.ticketId,
            assignment.id,
            assignment.state.requeue.includes(ticket.playerId)
          )
        )
        .submit();
  }

  async returned(report: ReturnReport): Promise<void> {
    const assignment = this.assignments.get(report.assignmentId);
    const ticket = assignment?.members.find((member) => member.ticketId === report.ticketId);
    if (
      assignment?.state.phase !== 'withdrawing' ||
      !ticket ||
      assignment.returned.includes(ticket.playerId)
    )
      return;
    assignment.returned.push(ticket.playerId);
    if (assignment.returned.length === assignment.members.length)
      await this.closeRoom(assignment.id);
  }

  private async closeRoom(id: string): Promise<void> {
    await this.context.outbound
      .sendToSpot(Names.roomProvisionerKey, new WithdrawRoom(id))
      .instanceSpot(Names.roomProvisioner)
      .inMesh(Names.mesh)
      .submit();
  }

  roomClosed(message: RoomClosed): void {
    const assignment = this.assignments.get(message.assignmentId);
    if (assignment?.state.phase !== 'withdrawing') return;
    const { requeue } = assignment.state;
    this.release(assignment);
    if (!requeue.length) return;
    const waiting = new Map(this.tickets);
    this.tickets.clear();
    for (const member of assignment.members) {
      if (requeue.includes(member.playerId)) this.tickets.set(member.playerId, member);
    }
    for (const [playerId, entry] of waiting) this.tickets.set(playerId, entry);
  }
}

class PoolTick implements ZLinkSpotTimerHandler<Pool> {
  async handle(spot: Pool): Promise<void> {
    await spot.advance();
  }
}

interface PoolSnapshot {
  tickets: (Omit<Ticket, 'waitingSince' | 'disconnectedAt'> & {
    waitingMs: number;
    disconnectedMs?: number;
  })[];
  assignments: (Omit<Assignment, 'deadline' | 'members'> & {
    remainingMs: number;
    members: [string, string];
  })[];
}

@Injectable()
export class PoolRelocation implements ZLinkSpotRelocationAdapter<Pool> {
  async capture(spot: Pool): Promise<Uint8Array> {
    const now = performance.now();
    const snapshot: PoolSnapshot = {
      tickets: Array.from(spot.tickets.values(), ({ waitingSince, disconnectedAt, ...ticket }) => ({
        ...ticket,
        waitingMs: now - waitingSince,
        disconnectedMs: disconnectedAt === undefined ? undefined : now - disconnectedAt
      })),
      assignments: Array.from(
        spot.assignments.values(),
        ({ deadline, members, ...assignment }) => ({
          ...assignment,
          remainingMs: Math.max(0, deadline - now),
          members: [members[0].playerId, members[1].playerId]
        })
      )
    };
    return Buffer.from(JSON.stringify(snapshot));
  }

  async restore(spot: Pool, payload: Uint8Array): Promise<void> {
    const snapshot = JSON.parse(Buffer.from(payload).toString()) as PoolSnapshot;
    const now = performance.now();
    for (const { waitingMs, disconnectedMs, ...ticket } of snapshot.tickets) {
      spot.tickets.set(ticket.playerId, {
        ...ticket,
        waitingSince: now - waitingMs,
        disconnectedAt: disconnectedMs === undefined ? undefined : now - disconnectedMs
      });
    }
    for (const {
      remainingMs,
      members: [first, second],
      ...assignment
    } of snapshot.assignments) {
      const firstTicket = spot.tickets.get(first);
      const secondTicket = spot.tickets.get(second);
      if (!firstTicket || !secondTicket)
        throw new Error('Pool snapshot is missing assigned tickets');
      spot.assignments.set(assignment.id, {
        ...assignment,
        deadline: now + remainingMs,
        members: [firstTicket, secondTicket]
      });
    }
  }
}
