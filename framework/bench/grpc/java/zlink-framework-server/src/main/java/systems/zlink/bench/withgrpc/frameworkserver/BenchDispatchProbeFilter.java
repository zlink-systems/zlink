/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.frameworkserver;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.ZLinkHandlerFilterContext;
import systems.zlink.framework.ZLinkHandlerFilterNext;

/**
 * Diagnostic only, enabled by {@code BENCH_DISPATCH_PROBE}. It is off in every measured
 * run and adds no cost there.
 *
 * <p>It exists because a route-mesh channel send whose handler never runs produces no
 * signal anywhere: the dispatcher traces {@code received}, {@code admitted} and
 * {@code dispatched}, then only traces again on success, so a failed invocation is
 * silent. This filter sits inside the handler invocation chain and reports whether the
 * chain was entered at all and how it ended, which is the difference between "the
 * handler was never resolved" and "the handler was resolved and then threw".
 */
public final class BenchDispatchProbeFilter implements ZLinkHandlerFilter {
    @Override
    public <T> CompletionStage<T> invoke(
        ZLinkHandlerFilterContext context, ZLinkHandlerFilterNext<T> next) {
        System.err.println("[probe] filter entered kind=" + context.dispatchKind());
        CompletionStage<T> stage;
        try {
            stage = next.invoke();
        } catch (Throwable immediate) {
            System.err.println("[probe] next.invoke threw synchronously: " + immediate);
            immediate.printStackTrace();
            throw immediate;
        }
        return stage.whenComplete((value, error) -> {
            if (error == null) {
                System.err.println("[probe] chain completed kind=" + context.dispatchKind());
            } else {
                System.err.println("[probe] chain FAILED kind=" + context.dispatchKind()
                    + " error=" + error);
                error.printStackTrace();
            }
        });
    }
}
