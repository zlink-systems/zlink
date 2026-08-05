/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.single;



import systems.zlink.perf.PerfUtil;
import java.util.Map;
import java.util.function.Function;

final class PerfPatternRegistry {
    private static final Map<String, Function<PerfUtil.Config, PerfUtil.Result>> PATTERNS =
        Map.of(
            "PAIR", PerfPair::run,
            "PUBSUB", PerfPubSub::run,
            "DEALER_DEALER", PerfDealerDealer::run,
            "DEALER_ROUTER", PerfDealerRouter::run,
            "DEALER_ROUTER_REQREP", config -> PerfSocketReqRep.run(config, false),
            "ROUTER_ROUTER", PerfRouterRouter::run,
            "ROUTER_ROUTER_REQREP", config -> PerfSocketReqRep.run(config, true)
        );

    private PerfPatternRegistry() {
    }

    static PerfUtil.Result run(PerfUtil.Config config) {
        Function<PerfUtil.Config, PerfUtil.Result> runner =
            PATTERNS.get(config.pattern());
        if (runner == null) {
            throw new IllegalArgumentException(
                "unsupported pattern: " + config.pattern());
        }
        return runner.apply(config);
    }
}
