/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;



import systems.zlink.perf.PerfUtil;
import java.util.Map;

final class PerfPatternRegistry {
    private interface PatternRunner {
        PerfUtil.Result run(PerfUtil.Config config, boolean serverRole);
    }

    private static final PatternRunner DEALER_ROUTER_SENDSEND =
        (config, serverRole) -> serverRole
            ? PerfMultiDealerRouter.runServer(config)
            : PerfMultiDealerRouter.runClient(config);
    private static final PatternRunner ROUTER_ROUTER_SENDSEND =
        (config, serverRole) -> serverRole
            ? PerfMultiRouterRouter.runServer(config)
            : PerfMultiRouterRouter.runClient(config);

    private static final Map<String, PatternRunner> PATTERNS = Map.ofEntries(
        Map.entry("DEALER_DEALER",
        (config, serverRole) -> serverRole
            ? PerfMultiDealerDealer.runServer(config)
            : PerfMultiDealerDealer.runClient(config)),
        // C calls these one-way echo scenarios SENDSEND. Accept the previous
        // Java spellings as input aliases, but report the C canonical key.
        Map.entry("DEALER_ROUTER_SENDSEND", DEALER_ROUTER_SENDSEND),
        Map.entry("DEALER_ROUTER", DEALER_ROUTER_SENDSEND),
        Map.entry("DEALER_ROUTER_REQREP",
        (config, serverRole) -> serverRole
            ? PerfMultiSocketReqRep.runServer(config)
            : PerfMultiSocketReqRep.runClient(config, false)),
        Map.entry("ROUTER_ROUTER_SENDSEND", ROUTER_ROUTER_SENDSEND),
        Map.entry("ROUTER_ROUTER", ROUTER_ROUTER_SENDSEND),
        Map.entry("ROUTER_ROUTER_REQREP",
        (config, serverRole) -> serverRole
            ? PerfMultiSocketReqRep.runServer(config)
            : PerfMultiSocketReqRep.runClient(config, true)),
        Map.entry("PUBSUB",
        (config, serverRole) -> serverRole
            ? PerfMultiPubSub.runServer(config)
            : PerfMultiPubSub.runClient(config)),
        Map.entry("STREAM",
        (config, serverRole) -> serverRole
            ? PerfMultiStream.runServer(config)
            : PerfMultiStream.runClient(config))
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
