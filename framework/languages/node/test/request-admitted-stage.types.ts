import type {
  ZLinkRequest,
  ZLinkRequestCall,
  ZLinkChannelRequestCall,
  ZLinkSpotRequestCall,
  ZLinkActorRequestCall
} from '../packages/framework/src';

export async function requestStageTypes(
  calls: readonly (
    ZLinkRequestCall | ZLinkChannelRequestCall | ZLinkSpotRequestCall | ZLinkActorRequestCall
  )[],
  signal: AbortSignal
): Promise<number[]> {
  const requests: ZLinkRequest<number>[] = calls.map((call) => call.submit<number>(signal));
  for (const request of requests) {
    const admission: Promise<void> = request.admitted;
    await admission;
    // @ts-expect-error The admission observation is readonly.
    request.admitted = Promise.resolve();
  }
  const replies: Promise<number>[] = requests;
  return Promise.all(replies);
}
