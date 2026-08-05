import { PlayActor } from './play-actor';
import { actorDisplayName } from '../../../../../Shared/Contracts/messages';
import { SampleDefaults } from '../../../../Configuration/sample-settings';
import type {
  ZLinkActor,
  ZLinkActorContext
} from '@zlink-systems/framework';
import type {
  TicTacToeActor
} from '../../../../../Shared/Contracts/messages';

type PlayZLinkActor = TicTacToeActor & ZLinkActor;

class PlayActorFactory {
  async create(context: ZLinkActorContext): Promise<PlayZLinkActor> {
    const actorId = context.actorId;
    return new PlayActor(
      actorId,
      actorDisplayName(actorId),
      context,
      SampleDefaults.requiredLevel,
      actorId === 'player-x' ? 99 : 0
    );
  }
}

export { PlayActorFactory };
