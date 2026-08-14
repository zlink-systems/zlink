import { SubmitResult } from './runtime-values';
import { ZLinkSubmitStatus } from '../messaging/submission-result';
import type {
  ZLinkBackendActorSessionNode,
  ZLinkBackendMeshNode
} from './contracts';
import {
  closeMeshCompletion,
  type ZLinkMeshCompletionTable
} from './mesh-completion-table';
import { routingIdsEqual } from '../routing-id';

export function meshActorSessionNodeAdapter(
  node: ZLinkBackendMeshNode,
  completions?: ZLinkMeshCompletionTable
): ZLinkBackendActorSessionNode {
  return {
    actorNodeGeneration(actor) {
      const status = node.status();
      return routingIdsEqual(status.routingId, actor.nodeRid)
        ? status.lifecycleGeneration
        : undefined;
    },
    async sendActorBoundSession(actor, expectedBindingGeneration, parts, flags, actorFence) {
      const result = await node.sendActorBoundSession(
        actor,
        expectedBindingGeneration,
        parts as never,
        flags,
        actorFence
      );
      switch (result) {
        case SubmitResult.Ok:
          return { status: ZLinkSubmitStatus.Submitted };
        case SubmitResult.Backpressured:
        case SubmitResult.NotAdmitted:
          return { status: ZLinkSubmitStatus.Backpressured };
        case SubmitResult.NotFound:
        case SubmitResult.InvalidState:
          return { status: ZLinkSubmitStatus.TargetNotFound };
        case SubmitResult.NotConnected:
          return { status: ZLinkSubmitStatus.RouteNotConnected };
        case SubmitResult.Terminated:
          return { status: ZLinkSubmitStatus.Shutdown };
        default:
          throw new Error(`Actor bound-session send failed with result '${result}'.`);
      }
    },
    async closeActorBoundSession(actor, expectedBindingGeneration, timeoutMs, signal) {
      if (completions === undefined) {
        throw new Error('MeshNode completion runtime is not started.');
      }
      const completion = await completions.submit(
        () => node.closeActorBoundSession(
          actor,
          expectedBindingGeneration,
          timeoutMs
        ),
        signal
      );
      try {
        if (completion.terminalResult !== 0 || completion.failureErrno !== 0) {
          throw new Error(
            `Actor '${actor.actorId}' bound-session close failed with result ` +
            `'${completion.terminalResult}' and errno '${completion.failureErrno}'.`
          );
        }
      } finally {
        closeMeshCompletion(completion);
      }
    }
  };
}
