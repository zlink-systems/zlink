import { Injectable } from '@nestjs/common';
import { zlinkRequestHandler } from '@zlink-systems/nestjs';
import {
  GetPlayerRecordRes,
  ReportBingoResultRes
} from '../../../Shared/Contracts/bingo-messages.generated';
import { PacketNames } from '../../../Shared/Contracts/messages';
import type {
  GetPlayerRecordReq,
  ReportBingoResultReq
} from '../../../Shared/Contracts/messages';
import type { ZLinkRequestHandler } from '@zlink-systems/framework';

type PlayerRecord = { readonly wins: number; readonly losses: number };

@Injectable()
class BingoPlayerRecordStore {
  private readonly records = new Map<string, PlayerRecord>();

  get(actorId: string): PlayerRecord {
    return this.records.get(actorId) ?? { wins: 0, losses: 0 };
  }

  report(actorId: string, won: boolean): PlayerRecord {
    const current = this.get(actorId);
    const updated = won
      ? { wins: current.wins + 1, losses: current.losses }
      : { wins: current.wins, losses: current.losses + 1 };
    this.records.set(actorId, updated);
    return updated;
  }
}

@zlinkRequestHandler('api', PacketNames.getPlayerRecordReq)
class GetPlayerRecordHandler implements ZLinkRequestHandler<GetPlayerRecordReq, GetPlayerRecordRes> {
  constructor(private readonly records: BingoPlayerRecordStore) {}

  async handle(request: GetPlayerRecordReq): Promise<GetPlayerRecordRes> {
    return new GetPlayerRecordRes({ actorId: request.actorId, ...this.records.get(request.actorId) });
  }
}

@zlinkRequestHandler('api', PacketNames.reportBingoResultReq)
class ReportBingoResultHandler implements ZLinkRequestHandler<ReportBingoResultReq, ReportBingoResultRes> {
  constructor(private readonly records: BingoPlayerRecordStore) {}

  async handle(request: ReportBingoResultReq): Promise<ReportBingoResultRes> {
    void request.roomId;
    void request.finalDrawSeq;
    return new ReportBingoResultRes({ actorId: request.actorId, ...this.records.report(request.actorId, request.won) });
  }
}

export { BingoPlayerRecordStore, GetPlayerRecordHandler, ReportBingoResultHandler };
