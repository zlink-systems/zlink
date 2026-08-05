import type { Message } from '../../contracts/Common/Message';
import {
  ZLinkConfigurationException
} from '../configuration';
import {
  isSpotRouteBridgeReplyPayload
} from '../spots/spot-route-reply-wire';
import {
  closeMessages
} from './channel-envelope';
import { createAbortError } from '../abort';

interface ZLinkPendingRawSpotRouteBridgeRequest {
  completed: boolean;
  timeout: ReturnType<typeof setTimeout> | undefined;
  abortHandler: (() => void) | undefined;
  resolve(reply: readonly Message[]): void;
  reject(error: unknown, releaseSubmission?: boolean): void;
  attachSubmission(resolve: () => void, reject: (error: unknown) => void): boolean;
}

/**
 * Owns the per-route-channel queue of in-flight raw SPOT route-bridge requests:
 * timeout/abort wiring, dequeue-on-completion, and matching arriving raw bridge
 * replies (FIFO) to the oldest pending request.
 */
export class ZLinkSpotRouteBridgeRawReplyRegistry {
  private readonly pending = new Map<string, ZLinkPendingRawSpotRouteBridgeRequest[]>();

  enqueue(
    routerChannelId: string,
    resolve: (reply: readonly Message[]) => void,
    reject: (error: unknown) => void,
    timeoutMs: number | undefined,
    defaultTimeoutMs: number | undefined,
    signal: AbortSignal | undefined
  ): ZLinkPendingRawSpotRouteBridgeRequest {
    let submissionComplete: (() => void) | undefined;
    let submissionFailed: ((error: unknown) => void) | undefined;
    const pending: ZLinkPendingRawSpotRouteBridgeRequest = {
      completed: false,
      timeout: undefined,
      abortHandler: undefined,
      attachSubmission: (complete, fail) => {
        if (pending.completed) {
          this.remove(routerChannelId, pending);
          complete();
          return false;
        }
        submissionComplete = complete;
        submissionFailed = fail;
        return true;
      },
      resolve: (reply) => {
        if (pending.completed) {
          closeMessages(reply);
          this.remove(routerChannelId, pending);
          submissionComplete?.();
          return;
        }
        pending.completed = true;
        this.remove(routerChannelId, pending);
        if (pending.timeout !== undefined) {
          clearTimeout(pending.timeout);
        }
        if (pending.abortHandler !== undefined) {
          signal?.removeEventListener('abort', pending.abortHandler);
        }
        submissionComplete?.();
        resolve(reply);
      },
      reject: (error, releaseSubmission = false) => {
        if (pending.completed) {
          if (releaseSubmission) {
            this.remove(routerChannelId, pending);
            submissionFailed?.(error);
          }
          return;
        }
        pending.completed = true;
        if (releaseSubmission) this.remove(routerChannelId, pending);
        if (pending.timeout !== undefined) {
          clearTimeout(pending.timeout);
        }
        if (pending.abortHandler !== undefined) {
          signal?.removeEventListener('abort', pending.abortHandler);
        }
        if (releaseSubmission) submissionFailed?.(error);
        reject(error);
      }
    };
    const queue = this.pending.get(routerChannelId) ?? [];
    queue.push(pending);
    this.pending.set(routerChannelId, queue);
    const effectiveTimeoutMs = timeoutMs ?? defaultTimeoutMs;
    if (effectiveTimeoutMs !== undefined) {
      pending.timeout = setTimeout(
        () => pending.reject(new ZLinkConfigurationException(`Route channel '${routerChannelId}' spot request timed out.`)),
        effectiveTimeoutMs
      );
    }
    if (signal !== undefined) {
      pending.abortHandler = () => pending.reject(createAbortError());
      signal.addEventListener('abort', pending.abortHandler, { once: true });
    }
    return pending;
  }

  remove(routerChannelId: string, pending: ZLinkPendingRawSpotRouteBridgeRequest): void {
    const queue = this.pending.get(routerChannelId);
    if (queue === undefined) {
      return;
    }
    const index = queue.indexOf(pending);
    if (index >= 0) {
      queue.splice(index, 1);
    }
    if (queue.length === 0) {
      this.pending.delete(routerChannelId);
    }
  }

  tryComplete(routerChannelId: string, received: { readonly parts: readonly Message[] }): boolean {
    const queue = this.pending.get(routerChannelId);
    if (queue === undefined || queue.length === 0 || !looksLikeRawSpotRouteBridgeReply(received.parts)) {
      return false;
    }
    queue[0].resolve(received.parts);
    return true;
  }

  rejectAll(error: unknown): void {
    const requests = [...this.pending.values()].flatMap((queue) => [...queue]);
    for (const request of requests) {
      request.reject(error, true);
    }
  }
}

function looksLikeRawSpotRouteBridgeReply(parts: readonly Message[]): boolean {
  if (parts.length === 0) {
    return false;
  }
  try {
    const decoded = JSON.parse(parts[0].data().toString('utf8')) as unknown;
    return isSpotRouteBridgeReplyPayload(decoded);
  } catch {
    return false;
  }
}
