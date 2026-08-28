/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletionStage;
import java.util.function.BiConsumer;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SendFlags;

/** Accepts further parts, a timeout, and the asynchronous request terminal. */
public interface RequestSubmitOperation
  extends MessageBuilderStage<RequestSubmitOperation>,
          TimeoutSubmitOperation<List<Message>> {
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
     * <p>The caller owns the returned messages and must close them.
     *
     * @return a future that completes with the reply message list
     */
    CompletionStage<List<Message>> submit();

    /** Blocks for admission and reply according to {@code flags}. */
    List<Message> submit_sync(SendFlags flags);

    /** Returns after admission and delivers reply completion to the callback. */
    void submit_sync(SendFlags flags,
                     BiConsumer<RequestResult, List<Message>> callback);
}
