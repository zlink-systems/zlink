import { randomUUID } from 'node:crypto';
import { performance } from 'node:perf_hooks';
import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT, ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import {
  ZLinkSpotCreateState,
  type ZLinkActor,
  type ZLinkActorClient,
  type ZLinkSpot,
  type ZLinkSpotContext,
  type ZLinkSpotManager,
  type SpotRef,
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
  WithdrawRoom,
  type CancelTicket,
  type EnterReport,
  type MatchPresence,
  type MatchRequest,
  type ReturnReport,
  type RoomPrepared,
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
export class Pool implements ZLinkSpot {
  readonly context!: ZLinkSpotContext<ZLinkActor, Pool>;
  readonly tickets = new Map<string, Ticket>();
  readonly assignments = new Map<string, Assignment>();

  constructor(
    @Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient
  ) {}

  async onActorJoin() {
    return { accepted: false };
  }

  async onJoinedActor(): Promise<void> {}

  async onLeaveActor(): Promise<void> {}

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
    for (const ticket of this.tickets.values()) {
      const reason = waitingCancellation(ticket, now);
      if (reason) await this.expire(ticket, reason);
    }
    for (const assignment of this.assignments.values()) {
      if (!assignment.withdrawal && decide(assignment, now) === 'withdraw')
        await this.withdraw(assignment);
    }
    const members = waitingPair(this.tickets.values());
    if (members) await this.prepare(members, now);
  }

  private async prepare(members: [Ticket, Ticket], now: number): Promise<void> {
    const id = Names.matchRoomPrefix + randomUUID();
    const deadline = now + Matchmaking.entryMs;
    const assignment: Assignment = { id, deadline, members, entered: [], returned: [] };
    for (const ticket of members) ticket.assignmentId = id;
    this.assignments.set(id, assignment);
    let room: SpotRef | undefined;
    let prepared: boolean;
    try {
      const created = await this.spots.getOrCreate(id, Names.table).inMesh(Names.mesh).submit();
      if (created.state === ZLinkSpotCreateState.Rejected) {
        this.releaseUnassigned(assignment);
        return;
      }
      room = created.spot;
      const reply = await this.context.outbound
        .requestToSpot(
          id,
          new PrepareRoom(
            id,
            members.map((ticket) => ticket.playerId)
          )
        )
        .submit<RoomPrepared>();
      prepared = reply.ok && reply.assignmentId === id && performance.now() < deadline;
    } catch (error) {
      this.releaseUnassigned(assignment);
      if (room) await this.spots.close(room);
      throw error;
    }
    if (!prepared) {
      this.releaseUnassigned(assignment);
      await this.spots.close(room);
      return;
    }
    for (const ticket of members) {
      await this.actors
        .sendToActor(ticket.playerId, new MatchAssigned(ticket.ticketId, id, id))
        .submit();
    }
  }

  private releaseUnassigned(assignment: Assignment): void {
    this.assignments.delete(assignment.id);
    for (const ticket of assignment.members) delete ticket.assignmentId;
  }

  async report(report: EnterReport): Promise<void> {
    const assignment = this.assignments.get(report.assignmentId);
    const ticket = assignment?.members.find((member) => member.ticketId === report.ticketId);
    if (
      !assignment ||
      !ticket ||
      assignment.withdrawal ||
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
      this.assignments.delete(assignment.id);
      for (const ticket of assignment.members) this.tickets.delete(ticket.playerId);
    }
  }

  private async withdraw(assignment: Assignment, failedPlayer?: string): Promise<void> {
    const requeue =
      failedPlayer === undefined
        ? assignment.entered
        : assignment.members
            .filter((ticket) => ticket.playerId !== failedPlayer)
            .map((ticket) => ticket.playerId);
    assignment.withdrawal = { requeue };
    await this.context.outbound.sendToSpot(assignment.id, new WithdrawRoom(assignment.id)).submit();
  }

  async roomWithdrawn(message: RoomWithdrawn): Promise<void> {
    const assignment = this.assignments.get(message.assignmentId);
    if (!assignment?.withdrawal) return;
    for (const ticket of assignment.members)
      await this.actors
        .sendToActor(
          ticket.playerId,
          new MatchReleased(
            ticket.ticketId,
            assignment.id,
            assignment.withdrawal.requeue.includes(ticket.playerId)
          )
        )
        .submit();
  }

  async returned(report: ReturnReport): Promise<void> {
    const assignment = this.assignments.get(report.assignmentId);
    const ticket = assignment?.members.find((member) => member.ticketId === report.ticketId);
    if (!assignment?.withdrawal || !ticket || assignment.returned.includes(ticket.playerId)) return;
    assignment.returned.push(ticket.playerId);
    if (assignment.returned.length !== assignment.members.length) return;
    const room = await this.spots.find(assignment.id);
    if (room && !(await this.spots.close(room))) throw new Error('Withdrawn room still occupied');
    this.assignments.delete(assignment.id);
    const waiting = new Map(this.tickets);
    this.tickets.clear();
    for (const member of assignment.members) {
      waiting.delete(member.playerId);
      if (assignment.withdrawal.requeue.includes(member.playerId)) {
        delete member.assignmentId;
        this.tickets.set(member.playerId, member);
      }
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
