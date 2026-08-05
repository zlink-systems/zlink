import { zlinkRequestHandler } from '@zlink-systems/nestjs';
import { PacketNames, SupportChatRoles } from '../../../Shared/Contracts/messages';
import type { ZLinkRequestHandler } from '@zlink-systems/framework';
import type { AuthenticateUserReq, AuthenticateUserRes } from '../../../Shared/Contracts/messages';

@zlinkRequestHandler('api', PacketNames.authenticateUserReq)
class AuthenticateUserHandler implements ZLinkRequestHandler<AuthenticateUserReq, AuthenticateUserRes> {
  async handle(request: AuthenticateUserReq): Promise<AuthenticateUserRes> {
    if (!/^(agent|customer)-[a-z0-9-]+$/i.test(request.accessToken)) {
      return { accepted: false, reason: 'Invalid support chat access token.' };
    }
    const role = request.accessToken.startsWith('agent-') ? SupportChatRoles.Agent : SupportChatRoles.Customer;
    return {
      accepted: true,
      actorId: request.accessToken,
      displayName: role === SupportChatRoles.Agent ? `Agent ${request.accessToken}` : `Customer ${request.accessToken}`,
      role
    };
  }
}

export { AuthenticateUserHandler };
