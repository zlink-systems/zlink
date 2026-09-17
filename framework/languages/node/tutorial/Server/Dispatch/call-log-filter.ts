import { Injectable, Logger } from '@nestjs/common';
import type {
  ZLinkHandlerFilter,
  ZLinkHandlerFilterContext,
  ZLinkHandlerFilterNext
} from '@zlink-systems/framework';

// --8<-- [start:filter-implementation]
// Runs around every handler this node receives, so the same logging is not
// repeated in each handler. Calling next() runs the handler; skipping it does
// not.
@Injectable()
export class CallLogFilter implements ZLinkHandlerFilter {
  private readonly logger = new Logger(CallLogFilter.name);

  async invoke(
    context: ZLinkHandlerFilterContext,
    next: ZLinkHandlerFilterNext,
    signal?: AbortSignal
  ): Promise<void> {
    void signal;
    const startedAt = Date.now();
    this.logger.log(`dispatch start: ${context.packetName}`);

    await next();

    // Everything after next() runs on the way back out, so the filters
    // unwind in reverse registration order.
    const elapsed = Date.now() - startedAt;
    this.logger.log(`dispatch done: ${context.packetName} in ${elapsed}ms`);
  }
}
// --8<-- [end:filter-implementation]
