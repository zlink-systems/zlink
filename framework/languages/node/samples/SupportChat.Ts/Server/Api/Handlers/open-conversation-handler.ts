import { Inject } from '@nestjs/common';
import { ZLINK_SPOT_MANAGER, zlinkRequestHandler } from '@zlink-systems/nestjs';
import { SampleNames } from '../../Configuration/sample-names';
import { ConversationStatuses, PacketNames } from '../../../Shared/Contracts/messages';
import type { ZLinkRequestHandler, ZLinkSpotManager } from '@zlink-systems/framework';
import type {
  OpenConversationApiReq,
  OpenConversationApiRes
} from '../../../Shared/Contracts/messages';

@zlinkRequestHandler('api', PacketNames.openConversationApiReq)
class OpenConversationHandler implements ZLinkRequestHandler<OpenConversationApiReq, OpenConversationApiRes> {
  constructor(@Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager) {}

  async handle(request: OpenConversationApiReq): Promise<OpenConversationApiRes> {
    const created = await this.spots
      .create(SampleNames.conversationSpotType)
      .inMesh(SampleNames.meshName)
      .request({
        customerActorId: request.customerActorId,
        customerDisplayName: request.customerDisplayName,
        subject: request.subject
      })
      .submit();
    return {
      conversationId: String(created.spot.spotId),
      status: ConversationStatuses.WaitingForAgent
    };
  }
}

export { OpenConversationHandler };
