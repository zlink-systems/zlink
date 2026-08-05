import type {
  ZLinkPeerLocation,
  ZLinkPeerLocationFilter,
} from './Models';

export interface ZLinkPeerLocationResolver {
  listLivePeers(filter: ZLinkPeerLocationFilter, signal?: AbortSignal): Promise<readonly ZLinkPeerLocation[]>;
}
