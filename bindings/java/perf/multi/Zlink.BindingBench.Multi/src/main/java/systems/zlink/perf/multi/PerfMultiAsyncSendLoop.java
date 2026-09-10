/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import java.util.Objects;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ExecutionException;
import systems.zlink.contracts.messaging.SendSubmission;
import systems.zlink.contracts.sockets.SubmitResult;

/** Classifies public asynchronous sends without inspecting stage state. */
final class PerfMultiAsyncSendLoop {
    private PerfMultiAsyncSendLoop() {
    }

    /**
     * Returns whether this exact submission must wait for admission.
     *
     * <p>The completed {@code admitted()} stage carried by {@code OK} is not
     * observed. Only {@code BACKPRESSURED} authorizes an asynchronous wait.</p>
     */
    static boolean isBackpressured(SendSubmission submission) {
        SubmitResult result = Objects.requireNonNull(submission,
            "send submission").result();
        if (result == SubmitResult.OK) {
            return false;
        }
        if (result == SubmitResult.BACKPRESSURED) {
            return true;
        }
        throw new IllegalStateException("async send returned " + result);
    }

    static Throwable completionCause(Throwable error) {
        Throwable current = error;
        while ((current instanceof CompletionException
                || current instanceof ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }
}
