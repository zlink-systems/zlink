import type { ZLinkActorContext } from './ZLinkActorContext';
import type { ZLinkActorJoinCompletion } from './ZLinkActorFactory';

export interface ZLinkActor {
  readonly context: ZLinkActorContext;
  configure?(): void;
  onJoinCompleted?(completion: ZLinkActorJoinCompletion): Promise<void>;
}
