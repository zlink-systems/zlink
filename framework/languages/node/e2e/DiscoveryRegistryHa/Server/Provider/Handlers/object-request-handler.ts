import {
  ZLinkPacket,
  type ZLinkInstanceSpot,
  type ZLinkInstanceSpotContext,
  type ZLinkSpotRequestHandler
} from '@zlink-systems/framework';
import { Injectable, Scope } from '@nestjs/common';
import { ObjectReq, type ObjectRes } from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';

let objectEvidence: EvidenceStore | undefined;

export function configureObjectEvidence(value: EvidenceStore): void {
  objectEvidence = value;
}

@Injectable({ scope: Scope.TRANSIENT })
export class Config6InstanceSpot implements ZLinkInstanceSpot {
  readonly context!: ZLinkInstanceSpotContext;

  configure(): void {
    objectEvidence?.add(`object-configure|rid=${objectEvidence.rid}`);
    try {
      this.context.handlers.addPacket(ObjectRequestHandler);
    } catch (error) {
      objectEvidence?.add(`object-configure-error|${error instanceof Error ? error.message : String(error)}`);
      throw error;
    }
  }

  async onInitialize(): Promise<void> {
    // The handler evidence records activation before dispatch so a failed
    // request distinguishes placement failure from packet registration.
    objectEvidence?.add(`object-initialized|rid=${objectEvidence.rid}`);
  }
}

@Injectable()
@ZLinkPacket('ObjectReq')
export class ObjectRequestHandler implements ZLinkSpotRequestHandler<ZLinkInstanceSpot, ObjectReq, ObjectRes> {
  async handle(_spot: ZLinkInstanceSpot, request: ObjectReq): Promise<ObjectRes> {
    const providerRid = objectEvidence?.rid ?? 'unknown';
    objectEvidence?.add(`object-request|rid=${providerRid}|spotId=${request.spotId}|operationId=${request.operationId}`);
    return {
      spotId: request.spotId,
      operationId: request.operationId,
      payload: request.payload,
      providerRid
    };
  }
}
