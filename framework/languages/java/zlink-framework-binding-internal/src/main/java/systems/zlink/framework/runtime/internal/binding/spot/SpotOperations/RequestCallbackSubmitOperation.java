/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestCallback;
import systems.zlink.contracts.sockets.SendFlags;
import java.time.Duration;

/**
 * Callback-submission stage of a request builder (reached after
 * {@link RequestSubmitOperation#flags}).
 */
interface RequestCallbackSubmitOperation
  extends MessageBuilderStage<RequestCallbackSubmitOperation> {
    /**
     * Adds another request part; consumed on a successful submit (see
     * {@link RequestOperation} for the ownership contract).
     *
     * @param part the request part
     * @return this operation for chaining
     */
    RequestCallbackSubmitOperation message(Message part);

    /**
     * Sets the request timeout, replacing any previous value.
     *
     * @param timeout the request timeout
     * @return this operation for chaining
     */
    RequestCallbackSubmitOperation timeout(Duration timeout);

    /**
     * Sets the send flags, replacing any previous flags.
     *
     * @param flags the send flags
     * @return this operation for chaining
     */
    RequestCallbackSubmitOperation flags(SendFlags flags);

    /**
     * Submits the request; the result and reply parts are delivered to
     * {@code callback} (see {@link RequestSubmitOperation#submit} for ownership).
     *
     * @param callback receives the result and reply parts
     * @return {@code true} when dispatched; {@code false} on back-pressure when
     *         {@link SendFlags#DONT_WAIT} is set
     */
    boolean submit(RequestCallback callback);
}
