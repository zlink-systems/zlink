import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import { ZLinkPacket, type ZLinkMessageContext, type ZLinkSpotManager, type ZLinkSpotPacketHandler, type ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import type {
  RemoteSpotAwaitMsg,
  RemoteSpotAwaitReq,
  AwaitMsg,
  AutomaticTurnDispatchRes
} from '../../../Shared/messages';
import { AwaitReq } from '../../../Shared/messages';
import { EvidenceStore } from '../Support/evidence-store';
import type { AwaitProbeSpot } from '../Spots/await-probe-spot';

@Injectable()
@ZLinkPacket('RemoteSpotAwaitReq')
export class RemoteSpotAwaitHandler implements ZLinkSpotRequestHandler<AwaitProbeSpot, RemoteSpotAwaitReq, AutomaticTurnDispatchRes> {
  constructor(
    private readonly evidence: EvidenceStore,
    @Inject(ZLINK_SPOT_MANAGER) private readonly spotHandles: ZLinkSpotManager
  ) {}

  async handle(
    spot: AwaitProbeSpot,
    request: RemoteSpotAwaitReq,
    context: ZLinkMessageContext
  ): Promise<AutomaticTurnDispatchRes> {
    void context;
    await runRemoteSpotAwait(this.evidence, this.spotHandles, spot, request);
    return reply('TD-F1', request.requestId, spot, 'remote-await-completed');
  }
}

@Injectable()
@ZLinkPacket('RemoteSpotAwaitMsg')
export class RemoteSpotAwaitCommandHandler implements ZLinkSpotPacketHandler<AwaitProbeSpot, RemoteSpotAwaitMsg> {
  constructor(
    private readonly evidence: EvidenceStore,
    @Inject(ZLINK_SPOT_MANAGER) private readonly spotHandles: ZLinkSpotManager
  ) {}

  async handle(
    spot: AwaitProbeSpot,
    request: RemoteSpotAwaitMsg,
    context: ZLinkMessageContext
  ): Promise<void> {
    void context;
    await runRemoteSpotAwait(this.evidence, this.spotHandles, spot, request);
  }
}

async function runRemoteSpotAwait(
  evidence: EvidenceStore,
  spotHandles: ZLinkSpotManager,
  spot: AwaitProbeSpot,
  request: RemoteSpotAwaitReq | RemoteSpotAwaitMsg
): Promise<void> {
  const terminator = request.terminator ?? 'async';
  evidence.add(
    `remote-${terminator}-started|rid=${evidence.rid}|spot=${spot.context.spotId}`
    + `|request=${request.requestId}|target=${request.targetSpotId}|handler=spot`
  );
  const targetSpot = await spotHandles.find(request.targetSpotId);
  if (targetSpot === undefined) {
    throw new Error(`Remote spot target ref is required for '${request.targetSpotId}'.`);
  }
  const call = spot.context.outbound
    .requestToSpot(targetSpot.spotId, Object.assign(new AwaitReq(), {
      requestId: request.requestId,
      delayMs: request.delayMs,
      correlationId: 'remote-spot',
      terminator
    }))
    .timeout(5000);
  evidence.add(
    `remote-${terminator}-${terminator === 'yield' ? 'released' : 'held'}|rid=${evidence.rid}`
    + `|spot=${spot.context.spotId}`
    + `|request=${request.requestId}|target=${request.targetSpotId}|handler=spot`
  );
  const targetReply = terminator === 'yield'
    ? await call.yield<AutomaticTurnDispatchRes>()
    : await call.submit<AutomaticTurnDispatchRes>();
  evidence.add(
    `remote-${terminator}-resumed|rid=${evidence.rid}|spot=${spot.context.spotId}`
    + `|request=${request.requestId}|target=${request.targetSpotId}|targetNode=${targetReply.nodeRid}|handler=spot`
  );
  evidence.add(
    `remote-${terminator}-completed|rid=${evidence.rid}|spot=${spot.context.spotId}`
    + `|request=${request.requestId}|target=${request.targetSpotId}|targetNode=${targetReply.nodeRid}|handler=spot`
  );
}

function reply(
  scenarioId: string,
  requestId: string,
  spot: AwaitProbeSpot,
  marker: string
): AutomaticTurnDispatchRes {
  return {
    scenarioId,
    requestId,
    spotId: String(spot.context.spotId),
    nodeRid: String(spot.context.nodeRid),
    marker
  };
}
