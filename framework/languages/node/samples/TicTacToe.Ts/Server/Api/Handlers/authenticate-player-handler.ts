import { authenticatePlayerRes } from '../../../Shared/Contracts/messages';
import { zlinkRequestHandler } from '@zlink-systems/nestjs';
import type { ZLinkRequestHandler } from '@zlink-systems/framework';
import { PacketNames } from '../../../Shared/Contracts/messages';
import type {
  AuthenticatePlayerReq,
  AuthenticatePlayerRes,
} from '../../../Shared/Contracts/messages';

// --8<-- [start:doc-request-handler]
@zlinkRequestHandler('api', PacketNames.authenticatePlayerReq)
class AuthenticatePlayerHandler implements ZLinkRequestHandler<AuthenticatePlayerReq, AuthenticatePlayerRes> {
  async handle(request: AuthenticatePlayerReq): Promise<AuthenticatePlayerRes> {
    if (typeof request.accessToken !== 'string' || request.accessToken.length === 0) {
      throw new Error('accessToken is required.');
    }
    return authenticatePlayerRes(request.accessToken);
  }
}
// --8<-- [end:doc-request-handler]

export { AuthenticatePlayerHandler };
