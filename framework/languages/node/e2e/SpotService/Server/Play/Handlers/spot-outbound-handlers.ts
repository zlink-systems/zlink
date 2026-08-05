import { Injectable } from '@nestjs/common';
import type {
  ZLinkMessageContext,
  ZLinkPublishMessageContext,
  ZLinkSpotPacketHandler,
  ZLinkSpotSubscriptionHandler
} from '@zlink-systems/framework';
import { ZLinkPacket } from '@zlink-systems/framework';
import type {
  ChannelEchoRes,
  SpotOutboundNegativeMsg,
  SpotOutboundMsg
} from '../../../Shared/messages';
import {
  ChannelEchoReq,
  ChannelNotify,
  MissingChannelNotify,
  MissingChannelReq,
  SpotMsg,
  SpotServiceNames,
  spotServicePacket
} from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import type { ScenarioUserSpot } from '../Spots/scenario-spots';

@Injectable()
@ZLinkPacket('SpotOutboundMsg')
export class SpotOutboundHandler implements ZLinkSpotPacketHandler<ScenarioUserSpot, SpotOutboundMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(
    spot: ScenarioUserSpot,
    request: SpotOutboundMsg,
    context: ZLinkMessageContext
  ): Promise<void> {
    void context;
    const echo = await spot.context.outbound
      .requestToChannel(SpotServiceNames.externalClientChannel,
        spotServicePacket(ChannelEchoReq, { value: request.marker }))
      .submit<ChannelEchoRes>();
    const notifyMarker = `notify-${request.marker}`;
    await spot.context.outbound
      .sendToChannel(SpotServiceNames.externalClientChannel,
        spotServicePacket(ChannelNotify, { marker: notifyMarker }))
      .submit();
    await spot.context.outbound
      .publish(SpotServiceNames.spotChannel, SpotServiceNames.spotEventTopic,
        spotServicePacket(SpotMsg, { marker: 'sm-c2-publish' }))
      .submit();
    this.evidence.add(
      `spot-outbound|rid=${this.evidence.rid}|spot=${spot.context.spotId}`
      + `|echo=${echo.value}|notify=${notifyMarker}`
    );
  }
}

@Injectable()
@ZLinkPacket('SpotOutboundNegativeMsg')
export class SpotOutboundNegativeHandler implements ZLinkSpotPacketHandler<ScenarioUserSpot, SpotOutboundNegativeMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(
    spot: ScenarioUserSpot,
    request: SpotOutboundNegativeMsg,
    context: ZLinkMessageContext
  ): Promise<void> {
    void context;
    let requestFailed = false;
    try {
      await spot.context.outbound
        .requestToChannel(SpotServiceNames.externalClientChannel,
          spotServicePacket(MissingChannelReq, { value: request.marker }))
        .timeout(2000)
        .submit<ChannelEchoRes>();
    } catch {
      requestFailed = true;
    }
    await spot.context.outbound
      .sendToChannel(SpotServiceNames.externalClientChannel,
        spotServicePacket(MissingChannelNotify, { marker: `missing-${request.marker}` }))
      .submit();
    this.evidence.add(
      `spot-outbound-negative|rid=${this.evidence.rid}|spot=${spot.context.spotId}|requestFailed=${requestFailed ? 'True' : 'False'}`
    );
  }
}

@Injectable()
export class SpotMsgHandler implements ZLinkSpotSubscriptionHandler<ScenarioUserSpot, SpotMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: ScenarioUserSpot, event: SpotMsg, context: ZLinkPublishMessageContext): Promise<void> {
    void context;
    this.evidence.add(`spot-msg|rid=${this.evidence.rid}|spot=${spot.context.spotId}|marker=${event.marker}`);
  }
}
