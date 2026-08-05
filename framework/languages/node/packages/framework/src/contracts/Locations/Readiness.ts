import type { RoutingId } from '../Common';
import type { ZLinkLocationRole } from './Values';

export interface ZLinkLocationReadiness {
  isPeerReady(
    meshName: string,
    role: ZLinkLocationRole,
    nodeRid?: RoutingId,
    signal?: AbortSignal
  ): Promise<boolean>;
}
