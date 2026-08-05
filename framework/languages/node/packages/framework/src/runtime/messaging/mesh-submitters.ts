import { ZLinkFrameworkInternalErrorKind, internalFrameworkErrorKind } from '../framework-errors-internal';
import {
  ZLinkFrameworkException
} from '../../contracts';
import {
  ZLinkSubmitStatus,
  type ZLinkSubmitResult
} from './submission-result';
import { ZLinkAsyncSubmitter } from './index';

interface ZLinkMeshSubmitterEntry {
  submitter: ZLinkAsyncSubmitter;
  wake?: () => void;
}

type ZLinkMeshSendTimeoutResolver = (meshName: string) => number;
type ZLinkMeshCapacityResolver = (meshName: string) => number;

class ZLinkMeshTerminalResult extends Error {
  constructor(readonly result: ZLinkSubmitResult) {
    super(`Mesh submit completed with '${result.status}'.`);
  }
}

/** Owns the bounded admission queues driven by MeshNode SEND_READY records. */
export class ZLinkMeshSubmitterRegistry {
  private readonly entries = new Map<string, ZLinkMeshSubmitterEntry>();
  private disposed = false;

  constructor(
    private readonly timeoutMs: number | ZLinkMeshSendTimeoutResolver,
    private readonly capacity: number | ZLinkMeshCapacityResolver
  ) {}

  async submit(
    meshName: string,
    attempt: () => ZLinkSubmitResult,
    signal?: AbortSignal,
    timeoutMs?: number
  ): Promise<ZLinkSubmitResult> {
    if (this.disposed) {
      return { status: ZLinkSubmitStatus.Shutdown };
    }
    const entry = this.requireEntry(meshName);
    try {
      await entry.submitter.submitCommand(() => {
        const result = attempt();
        if (result.status === ZLinkSubmitStatus.Submitted) return true;
        if (result.status === ZLinkSubmitStatus.Backpressured) return false;
        throw new ZLinkMeshTerminalResult(result);
      }, signal, undefined, timeoutMs);
      return { status: ZLinkSubmitStatus.Submitted };
    } catch (error) {
      if (error instanceof ZLinkMeshTerminalResult) return error.result;
      if (error instanceof ZLinkFrameworkException) {
        switch (internalFrameworkErrorKind(error)) {
          case ZLinkFrameworkInternalErrorKind.RuntimeShutdown:
            return { status: ZLinkSubmitStatus.Shutdown };
          case ZLinkFrameworkInternalErrorKind.DeadlineExceeded:
          case ZLinkFrameworkInternalErrorKind.WorkerTimedOut:
            return { status: ZLinkSubmitStatus.TimedOut };
          case ZLinkFrameworkInternalErrorKind.WorkerQueueFull:
            return { status: ZLinkSubmitStatus.Backpressured };
        }
      }
      throw error;
    }
  }

  notify(meshName: string): void {
    if (this.disposed) return;
    this.entries.get(meshName)?.wake?.();
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    for (const entry of this.entries.values()) entry.submitter.dispose();
    this.entries.clear();
  }

  private requireEntry(meshName: string): ZLinkMeshSubmitterEntry {
    let entry = this.entries.get(meshName);
    if (entry !== undefined) return entry;
    entry = {
      submitter: undefined as unknown as ZLinkAsyncSubmitter
    };
    entry.submitter = new ZLinkAsyncSubmitter(
      (handler) => { entry!.wake = handler; },
      {
        timeoutMs: typeof this.timeoutMs === 'function'
          ? this.timeoutMs(meshName)
          : this.timeoutMs,
        capacity: typeof this.capacity === 'function'
          ? this.capacity(meshName)
          : this.capacity
      }
    );
    this.entries.set(meshName, entry);
    return entry;
  }
}
