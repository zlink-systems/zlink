import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import type { ZLinkActorRoutedJoinTransport } from './actor-routed-join-transport';

export async function tryRequestRoutedJson(
  transport: ZLinkActorRoutedJoinTransport,
  target: ZLinkSpotRouteTarget,
  payload: unknown,
  options: { readonly timeoutMs?: number; readonly signal?: AbortSignal }
): Promise<boolean> {
  const request = transport.requestRawToSpot;
  if (request === undefined) return false;
  const message = RuntimeMessage.from(Buffer.from(JSON.stringify(payload)));
  try {
    const replyParts = await request.call(transport, target, message, options);
    for (const part of replyParts) part.close();
    return true;
  } finally {
    message.close();
  }
}

export async function requestRoutedJson(
  transport: ZLinkActorRoutedJoinTransport,
  target: ZLinkSpotRouteTarget,
  payload: unknown,
  options: { readonly timeoutMs?: number; readonly signal?: AbortSignal },
  unavailableMessage: string
): Promise<void> {
  if (!await tryRequestRoutedJson(transport, target, payload, options)) {
    throw new Error(unavailableMessage);
  }
}

export async function requestRoutedJsonReply<T>(
  transport: ZLinkActorRoutedJoinTransport,
  target: ZLinkSpotRouteTarget,
  payload: unknown,
  options: { readonly timeoutMs?: number; readonly signal?: AbortSignal },
  unavailableMessage: string,
  decode: (replyParts: readonly Message[]) => T
): Promise<T> {
  const request = transport.requestRawToSpot;
  if (request === undefined) throw new Error(unavailableMessage);
  const message = RuntimeMessage.from(Buffer.from(JSON.stringify(payload)));
  try {
    const replyParts = await request.call(transport, target, message, options);
    try {
      return decode(replyParts);
    } finally {
      for (const part of replyParts) part.close();
    }
  } finally {
    message.close();
  }
}
