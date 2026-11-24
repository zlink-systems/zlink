import { randomUUID } from 'node:crypto';
import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_SPOT_OUTBOUND } from '@zlink-systems/nestjs';
import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorFactory,
  ZLinkActorJoinCompletion,
  ZLinkActorRelocationAdapter,
  ZLinkSpotOutbound
} from '@zlink-systems/framework';
import { MatchCancellation, Names, Reasons } from '../../shared/constants';
import {
  ActionRejected,
  CancelTicket,
  EnterReport,
  MatchCancelled,
  MatchFound,
  type MatchCommitted,
  MatchPresence,
  MatchQueued,
  MatchRequest,
  ReturnReport,
  RoomAdmission,
  type MatchAssigned,
  type MatchReleased
} from '../../shared/messages';

interface PlayerMatch {
  requestKey: string;
  ticketId?: string;
  assignment?: {
    id: string;
    phase: 'joining' | 'entered' | 'failed' | 'returning';
    requeue?: boolean;
  };
}

export class Player implements ZLinkActor {
  match?: PlayerMatch;

  constructor(
    readonly context: ZLinkActorContext,
    private readonly outbound: ZLinkSpotOutbound
  ) {}

  async findMatch(outbound: ZLinkSpotOutbound): Promise<void> {
    if (this.match?.ticketId) {
      await this.context.boundSession.send(new MatchQueued(this.match.ticketId)).submit();
      return;
    }
    this.match ??= { requestKey: randomUUID() };
    const reply = await outbound
      .requestToSpot(Names.poolKey, new MatchRequest(this.match.requestKey, this.context.actorId))
      .submit<MatchQueued>();
    this.match.ticketId = reply.ticketId;
    await this.context.boundSession.send(new MatchQueued(reply.ticketId)).submit();
  }

  async cancelMatch(outbound: ZLinkSpotOutbound, ticketId: string): Promise<void> {
    if (!ticketId || this.match?.ticketId !== ticketId) {
      await this.reject(Reasons.matchNotWaiting);
      return;
    }
    const reply = await this.cancelTicket(outbound, ticketId);
    if (reply) await this.context.boundSession.send(reply).submit();
    else await this.reject(Reasons.matchNotWaiting);
  }

  async disconnected(outbound: ZLinkSpotOutbound): Promise<void> {
    if (this.match?.ticketId)
      await outbound
        .sendToSpot(Names.poolKey, new MatchPresence(this.match.ticketId, false))
        .submit();
  }

  async onSessionBound(): Promise<void> {
    if (this.match?.ticketId)
      await this.outbound
        .sendToSpot(Names.poolKey, new MatchPresence(this.match.ticketId, true))
        .submit();
  }

  private async cancelTicket(
    outbound: ZLinkSpotOutbound,
    ticketId: string
  ): Promise<MatchCancelled | undefined> {
    const reply = await outbound
      .requestToSpot(Names.poolKey, new CancelTicket(ticketId))
      .submit<MatchCancelled>();
    if (reply.reason === Reasons.matchNotWaiting || reply.ticketId !== this.match?.ticketId)
      return undefined;
    this.match = undefined;
    return new MatchCancelled(reply.ticketId, reply.reason);
  }

  assign(message: MatchAssigned): void {
    const match = this.match;
    if (!match || match.ticketId !== message.ticketId || match.assignment) return;
    match.assignment = { id: message.assignmentId, phase: 'joining' };
    this.context.joinSpot(message.roomId, new RoomAdmission(message.assignmentId)).defer();
  }

  async committed(message: MatchCommitted): Promise<void> {
    const match = this.match;
    if (
      !match?.ticketId ||
      match.assignment?.id !== message.assignmentId ||
      match.assignment.phase !== 'entered'
    )
      return;
    this.match = undefined;
    await this.context.boundSession
      .send(new MatchFound(match.ticketId, message.assignmentId, message.seat))
      .submit();
  }

  async release(message: MatchReleased): Promise<void> {
    const match = this.match;
    if (
      !match ||
      match.ticketId !== message.ticketId ||
      match.assignment?.id !== message.assignmentId ||
      match.assignment.requeue !== undefined
    )
      return;
    match.assignment.requeue = message.requeue;
    await this.returnToEntry(match, match.assignment);
  }

  async ended(message: MatchCancelled): Promise<void> {
    if (this.match?.ticketId !== message.ticketId) return;
    this.match = undefined;
    if (message.reason === MatchCancellation.disconnected) return;
    await this.context.boundSession.send(message).submit();
  }

  async onJoinCompleted(completion: ZLinkActorJoinCompletion): Promise<void> {
    const match = this.match;
    const assignment = match?.assignment;
    if (!match || !assignment) {
      if (completion.status === 'failed') throw new Error(`Actor join failed: ${completion.kind}`);
      if (completion.status === 'rejected') await this.reject(Reasons.full);
      return;
    }
    if (assignment.phase === 'returning') {
      if (completion.status !== 'accepted') throw new Error('Could not return to Entry');
      await this.reportReturn(match, assignment);
      return;
    }
    if (assignment.phase !== 'joining') return;
    const ok = completion.status === 'accepted';
    assignment.phase = ok ? 'entered' : 'failed';
    await this.outbound
      .sendToSpot(Names.poolKey, new EnterReport(match.ticketId!, assignment.id, ok))
      .submit();
    await this.returnToEntry(match, assignment);
  }

  private async returnToEntry(
    match: PlayerMatch,
    assignment: NonNullable<PlayerMatch['assignment']>
  ): Promise<void> {
    if (
      assignment.requeue === undefined ||
      assignment.phase === 'joining' ||
      assignment.phase === 'returning'
    )
      return;
    if (assignment.phase === 'entered') {
      assignment.phase = 'returning';
      this.context.joinEntrySpot().defer();
    } else {
      await this.reportReturn(match, assignment);
    }
  }

  private async reportReturn(
    match: PlayerMatch,
    assignment: NonNullable<PlayerMatch['assignment']>
  ): Promise<void> {
    await this.outbound
      .sendToSpot(Names.poolKey, new ReturnReport(match.ticketId!, assignment.id))
      .submit();
    if (assignment.requeue) {
      delete match.assignment;
    } else {
      this.match = undefined;
      await this.context.boundSession
        .send(new MatchCancelled(match.ticketId!, MatchCancellation.cancelled))
        .submit();
    }
  }

  async reject(reason: string): Promise<void> {
    await this.context.boundSession.send(new ActionRejected(reason, 0)).submit();
  }
}

@Injectable()
export class PlayerFactory implements ZLinkActorFactory<Player> {
  constructor(@Inject(ZLINK_SPOT_OUTBOUND) private readonly outbound: ZLinkSpotOutbound) {}

  async create(context: ZLinkActorContext): Promise<Player> {
    return new Player(context, this.outbound);
  }
}

@Injectable()
export class PlayerRelocation implements ZLinkActorRelocationAdapter<Player> {
  async capture(actor: Player): Promise<Uint8Array> {
    return actor.match ? Buffer.from(JSON.stringify(actor.match)) : new Uint8Array();
  }

  async restore(actor: Player, payload: Uint8Array): Promise<void> {
    actor.match = payload.length
      ? (JSON.parse(Buffer.from(payload).toString()) as PlayerMatch)
      : undefined;
  }
}
