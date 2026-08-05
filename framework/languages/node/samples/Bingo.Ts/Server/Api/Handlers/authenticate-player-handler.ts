import { PacketNames, actorDisplayName } from '../../../Shared/Contracts/messages';
import { AuthenticatePlayerRes } from '../../../Shared/Contracts/bingo-messages.generated';
import { SampleNames } from '../../Configuration/sample-names';
import { zlinkRequestHandler } from '@zlink-systems/nestjs';
import type { ZLinkRequestHandler } from '@zlink-systems/framework';
import type {
  AuthenticatePlayerReq
} from '../../../Shared/Contracts/messages';

@zlinkRequestHandler('api', PacketNames.authenticatePlayerReq)
class AuthenticatePlayerHandler implements ZLinkRequestHandler<AuthenticatePlayerReq, AuthenticatePlayerRes> {
  async handle(request: AuthenticatePlayerReq): Promise<AuthenticatePlayerRes> {
    if (!(SampleNames.actorIds as readonly string[]).includes(request.accessToken)) {
      return new AuthenticatePlayerRes({
        accepted: false, actorId: null, displayName: null,
        reason: 'Access token must be a sample player id.'
      });
    }

    return new AuthenticatePlayerRes({
      accepted: true,
      actorId: request.accessToken,
      displayName: actorDisplayName(request.accessToken),
      reason: null
    });
  }
}

export { AuthenticatePlayerHandler };
