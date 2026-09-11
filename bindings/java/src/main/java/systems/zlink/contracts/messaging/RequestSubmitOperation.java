/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import java.time.Duration;
import java.util.List;

/** Accepts further parts, a timeout, and the asynchronous request terminal. */
public interface RequestSubmitOperation
  extends MessageBuilderStage<RequestSubmitOperation>,
          TimeoutSubmitOperation<RequestSubmission> {
    /**
     * Adds another request part; consumed on a successful submit (see
     * {@link RequestOperation} for the ownership contract).
     *
     * @param part the request part
     * @return this operation for chaining
     */
    RequestSubmitOperation message(Message part);

    /**
     * Sets how long the request may remain pending before timing out.
     *
     * @param timeout the request timeout; replaces any previous value
     * @return this operation for chaining
     */
    RequestSubmitOperation timeout(Duration timeout);

    /**
     * Submits the request and asynchronously returns the reply parts.
     *
     * <p>Each admission attempt uses Core DONTWAIT. Immediate admission starts
     * the reply timeout. On BACKPRESSURED/EAGAIN, Core retains only a wait
     * token; the binding retains the request and resubmits it only after the
     * matching WRITABLE completion. The reply stage then completes from the
     * ordinary REQUEST reply, timeout, or typed terminal completion. The
     * caller owns the returned messages and must close them.
     *
     * @return the initial result, admission stage, and reply stage
     */
    RequestSubmission submit();

    /** Blocks with Core NONE until the reply or typed terminal error. */
    List<Message> submit_sync();
}
