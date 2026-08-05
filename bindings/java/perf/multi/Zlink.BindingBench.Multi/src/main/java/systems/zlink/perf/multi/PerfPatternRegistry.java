/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;



import systems.zlink.perf.PerfUtil;
import java.util.Map;

final class PerfPatternRegistry {
    private interface PatternRunner {
        PerfUtil.Result run(PerfUtil.Config config, boolean serverRole);
    }

    private static final Map<String, PatternRunner> PATTERNS = Map.of(
        "DEALER_DEALER",
        (config, serverRole) -> serverRole
            ? PerfMultiDealerDealer.runServer(config)
            : PerfMultiDealerDealer.runClient(config),
        "DEALER_ROUTER",
        (config, serverRole) -> serverRole
            ? PerfMultiDealerRouter.runServer(config)
            : PerfMultiDealerRouter.runClient(config),
        "DEALER_ROUTER_REQREP",
        (config, serverRole) -> serverRole
            ? PerfMultiSocketReqRep.runServer(config)
            : PerfMultiSocketReqRep.runClient(config, false),
        "ROUTER_ROUTER",
        (config, serverRole) -> serverRole
            ? PerfMultiRouterRouter.runServer(config)
            : PerfMultiRouterRouter.runClient(config),
        "ROUTER_ROUTER_REQREP",
        (config, serverRole) -> serverRole
            ? PerfMultiSocketReqRep.runServer(config)
            : PerfMultiSocketReqRep.runClient(config, true),
        "PUBSUB",
        (config, serverRole) -> serverRole
            ? PerfMultiPubSub.runServer(config)
            : PerfMultiPubSub.runClient(config),
        "STREAM",
        (config, serverRole) -> serverRole
            ? PerfMultiStream.runServer(config)
            : PerfMultiStream.runClient(config)
    );

    private PerfPatternRegistry() {
    }

    static PerfUtil.Result run(PerfUtil.Config config, boolean serverRole) {
        PatternRunner runner = PATTERNS.get(config.pattern());
        if (runner == null) {
            throw new IllegalArgumentException(
                "unsupported pattern: " + config.pattern());
        }
        return runner.run(config, serverRole);
    }
}
