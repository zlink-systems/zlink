/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import java.util.List;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.sockets.SubmitResult;

/** Snapshot, admission completion, and reply for an asynchronous request. */
public interface RequestSubmission {
    /** Returns the immutable OK or BACKPRESSURED initial result. */
    SubmitResult result();

    /** Completes when the request is admitted to the local send queue. */
    CompletionStage<Void> admitted();

    /** Completes with the reply after successful admission. */
    CompletionStage<List<Message>> reply();
}
