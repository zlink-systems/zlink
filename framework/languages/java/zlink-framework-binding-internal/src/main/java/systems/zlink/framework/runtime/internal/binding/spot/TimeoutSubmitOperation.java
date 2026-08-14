/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

/** Common stage for builders that set a timeout and then submit. */
interface TimeoutSubmitOperation<TResult> {
    TimeoutSubmitOperation<TResult> timeout(Duration timeout);

    CompletionStage<TResult> submit();
}
