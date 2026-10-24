import { wireReplyFailureException } from '../framework-errors-internal';
import { classifySubmitResult } from '../messaging/submission-result';
import type { ZLinkBackendActorSessionNode, ZLinkBackendMeshNode } from './contracts';
import { closeMeshCompletion, type ZLinkMeshCompletionTable } from './mesh-completion-table';

export function meshActorSessionNodeAdapter(
  node: ZLinkBackendMeshNode,
  completions?: ZLinkMeshCompletionTable
): ZLinkBackendActorSessionNode {
  return {
    async sendActorBoundSession(actor, expectedBindingGeneration, parts, flags) {
      const result = await node.sendActorBoundSession(
        actor,
        expectedBindingGeneration,
        parts as never,
        flags
      );
      return classifySubmitResult(result, 'Actor bound-session send');
    },
    async closeActorBoundSession(actor, expectedBindingGeneration, timeoutMs, signal) {
      if (completions === undefined) {
        throw new Error('MeshNode completion runtime is not started.');
      }
      const completion = await completions.submit(
        () => node.closeActorBoundSession(actor, expectedBindingGeneration, timeoutMs),
        signal
      ).pending;
      try {
        if (completion.terminalResult !== 0 || completion.failureErrno !== 0) {
          throw wireReplyFailureException(
            completion.terminalResult,
            completion.failureErrno,
            `Actor '${actor.actorId}' bound-session close failed.`
          );
        }
      } finally {
        closeMeshCompletion(completion);
      }
    }
  };
}
