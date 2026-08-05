import type { ZLinkFanoutClient } from '@zlink-systems/framework';
import { EventMsg, MissingEventMsg, PubSubNames } from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import type { HttpRoute } from '../Support/http-server';

export function createPublisherEndpoints(
  fanout: ZLinkFanoutClient,
  evidence: EvidenceStore,
  stop: () => void,
  channelName: string = PubSubNames.channel
): readonly HttpRoute[] {
  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role: 'publisher', rid: evidence.rid }) },
    { method: 'GET', path: '/status/listener', handle: () => fanout.getListenerStatus(channelName) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    { method: 'POST', path: '/evidence/clear', handle: () => { evidence.clear(); return { status: 'cleared' }; } },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping' }; } },
    {
      method: 'POST', path: '/admin/drain', handle: () => {
        stop();
        return { kind: 'drained' as const };
      }
    },
    {
      method: 'POST',
      path: '/publish/event',
      handle: async (body) => {
        const request = body as PublishRequest;
        const event = new EventMsg(request.runId, Number(request.sequence), request.value);
        await fanout.publish(channelName, request.topic, event).submit();
        return { status: 'published', topic: request.topic, runId: request.runId, sequence: event.sequence };
      }
    },
    {
      method: 'POST',
      path: '/publish/missing',
      handle: async (body) => {
        const request = body as PublishRequest;
        const event = new MissingEventMsg(request.runId, Number(request.sequence), request.value);
        await fanout.publish(channelName, request.topic, event).submit();
        return { status: 'published', topic: request.topic, runId: request.runId, sequence: event.sequence };
      }
    }
  ];
}

interface PublishRequest {
  readonly topic: string;
  readonly runId: string;
  readonly sequence: number;
  readonly value: string;
}
