/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import systems.zlink.contracts.sockets.RequestCallback;
import systems.zlink.contracts.sockets.SendFlags;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletionStage;

/** Accepts further parts, timeout, flags, and the terminal submit of a request. */
public interface RequestSubmitOperation
  extends MessageBuilderStage<RequestSubmitOperation>,
          TimeoutSubmitOperation<List<Message>, RequestCallback> {
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
     * Sets the send flags and narrows to callback submission; the async
     * {@link #submit()} path is no longer reachable after this call.
     *
     * @param flags the send flags
     * @return the callback-submit stage
     */
    RequestCallbackSubmitOperation flags(SendFlags flags);

    /**
     * Submits the request and asynchronously returns the reply parts.
     *
     * <p>The caller owns the returned messages and must close them.
     *
     * @return a future that completes with the reply message list
     */
    CompletionStage<List<Message>> submit();

    /**
     * Submits the request; the result and reply parts are delivered to
     * {@code callback}.
     *
     * @param callback receives the result and reply parts; owns the parts on
     *                 {@link systems.zlink.contracts.sockets.RequestResult#OK OK}
     * @return {@code true} when dispatched; {@code false} only when
     *         {@link SendFlags#DONT_WAIT} is set and the send would have
     *         blocked; other failures throw
     *         {@link systems.zlink.contracts.errors.ZlinkException}
     */
    boolean submit(RequestCallback callback);
}
