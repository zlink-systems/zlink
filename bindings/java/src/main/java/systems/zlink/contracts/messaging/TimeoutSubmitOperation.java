/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

/** Common stage for builders that set a timeout and then submit. */
public interface TimeoutSubmitOperation<TResult> {
    TimeoutSubmitOperation<TResult> timeout(Duration timeout);

    CompletionStage<TResult> submit();
}
