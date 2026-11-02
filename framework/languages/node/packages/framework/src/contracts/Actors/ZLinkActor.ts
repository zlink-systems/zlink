import type { ZLinkActorContext } from './ZLinkActorContext';
import type { ZLinkActorJoinCompletion } from './ZLinkActorFactory';
import type { ZLinkSessionBindingIdentity } from './ZLinkSessionBindingIdentity';

export interface ZLinkActor {
  readonly context: ZLinkActorContext;
  configure?(): void;
  onJoinCompleted?(completion: ZLinkActorJoinCompletion): Promise<void>;
  onDuplicateBind?(
    current: ZLinkSessionBindingIdentity,
    incoming: ZLinkSessionBindingIdentity,
    signal: AbortSignal
  ): Promise<string | undefined>;
}
