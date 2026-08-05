import type { ZLinkRouteClient } from '@zlink-systems/framework';
import { EchoJsonReq, RegistrationCodecNames, type EchoRes } from '../../../Shared/messages';
import type { HttpRoute } from '../Support/http-server';

export function createCodecRequesterEndpoints(channel: ZLinkRouteClient): readonly HttpRoute[] {
  return [
    {
      method: 'POST',
      path: '/codec/mismatch',
      handle: async () => {
        let rejected = false;
        let failureType: string | undefined;
        try {
          await channel.requestToChannel(RegistrationCodecNames.channel, new EchoJsonReq('rc-b5-protobuf'))
            .submit<EchoRes>();
        } catch (error) {
          rejected = true;
          failureType = error instanceof Error ? error.constructor.name : typeof error;
        }
        if (!rejected) {
          throw new Error('RC-B5 expected Protobuf request to JSON-only peer to fail.');
        }

        return {
          rejected,
          failureType,
          value: null,
          contentType: null
        };
      }
    }
  ];
}
