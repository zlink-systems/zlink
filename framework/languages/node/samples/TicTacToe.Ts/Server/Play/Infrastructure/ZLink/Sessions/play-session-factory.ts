import { PlaySession } from './play-session';
import { AuthenticatePlaySessionHandler } from './Handlers/authenticate-play-session-handler';
import type {
  ZLinkSessionContext,
  ZLinkSessionFactory
} from '@zlink-systems/framework';
import type { PlaySession as PlaySessionType } from './play-session';
class PlaySessionFactory implements ZLinkSessionFactory<PlaySessionType> {
  async create(context: ZLinkSessionContext): Promise<PlaySessionType> {
    // request: AuthenticateReq returns AuthenticateRes before Actor relay is enabled.
    context.handlers.addHandler(AuthenticatePlaySessionHandler);
    return new PlaySession(context);
  }
}

export { PlaySessionFactory };
