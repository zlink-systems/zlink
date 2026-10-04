import type { ZLinkMeshNodeDescriptor } from '../../contracts/Locations/Models';
import type { ZLinkMeshNodeLocationStore } from './internal-store-contracts';

export async function listAllMeshNodeDescriptors(
  store: ZLinkMeshNodeLocationStore,
  meshName: string,
  signal?: AbortSignal,
  pageSize?: number
): Promise<readonly ZLinkMeshNodeDescriptor[]> {
  const rows: ZLinkMeshNodeDescriptor[] = [];
  let continuationToken: string | undefined;
  do {
    const result = await store.listMeshNodes(meshName, { pageSize, continuationToken }, signal);
    rows.push(...result.items);
    continuationToken = result.continuationToken;
  } while (continuationToken !== undefined);
  return rows;
}
