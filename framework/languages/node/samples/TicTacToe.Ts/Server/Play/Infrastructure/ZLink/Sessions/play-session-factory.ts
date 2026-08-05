import { PlaySession } from './play-session';
import type {
  ZLinkSessionContext,
  ZLinkSessionFactory
} from '@zlink-systems/framework';
import type { PlaySession as PlaySessionType } from './play-session';
class PlaySessionFactory implements ZLinkSessionFactory<PlaySessionType> {
  async create(context: ZLinkSessionContext): Promise<PlaySessionType> {
    return new PlaySession(context);
  }
}

export { PlaySessionFactory };
