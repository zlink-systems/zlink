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

export function meshActorSessionNodeAdapter(
  node: ZLinkBackendMeshNode,
  completions?: ZLinkMeshCompletionTable
): ZLinkBackendActorSessionNode {
  return {
    sendActorBoundSession(actor, expectedBindingGeneration, parts, flags) {
      const result = node.sendActorBoundSession(
        actor,
        expectedBindingGeneration,
        parts as never,
        flags
      ) as unknown as SubmitResult;
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
      const operationId = node.closeActorBoundSession(
        actor,
        expectedBindingGeneration,
        timeoutMs
      );
      const completion = await completions.wait(operationId, signal);
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
