using System.Collections.Generic;

internal static class SinglePerfPatternRegistry
{
    private static readonly PerfPatternRegistry Registry = new(
        new IPerfPattern[]
        {
            new SinglePerfPattern("PAIR", static options =>
                PerfPair.RunPair(options.Transport, options.Size)),
            new SinglePerfPattern("PUBSUB", static options =>
                PerfPubSub.RunPubSub(options.Transport, options.Size)),
            new SinglePerfPattern("DEALER_DEALER", static options =>
                PerfDealerDealer.RunDealerDealer(options.Transport, options.Size)),
            new SinglePerfPattern("DEALER_ROUTER", static options =>
                PerfDealerRouter.RunDealerRouter(options.Transport, options.Size)),
            new SinglePerfPattern("DEALER_ROUTER_REQREP", static options =>
                PerfReqRep.RunDealerRouter(options.Transport, options.Size)),
            new SinglePerfPattern("ROUTER_ROUTER", static options =>
                PerfRouterRouter.RunRouterRouter(options.Transport, options.Size)),
            new SinglePerfPattern("ROUTER_ROUTER_REQREP", static options =>
                PerfReqRep.RunRouterRouter(options.Transport, options.Size)),
        });

    internal static bool TryGet(string pattern, out IPerfPattern perfPattern)
    {
        return Registry.TryGet(pattern, out perfPattern);
    }

    private sealed class SinglePerfPattern : IPerfPattern
    {
        private readonly System.Func<PerfOptions, int> _runSingle;

        internal SinglePerfPattern(string name,
            System.Func<PerfOptions, int> runSingle)
        {
            Name = name;
            _runSingle = runSingle;
        }

        public string Name { get; }

        public int RunSingle(PerfOptions options)
        {
            return _runSingle(options);
        }

        public int RunMultiServer(PerfOptions options)
        {
            _ = options;
            return 1;
        }

        public int RunMultiClient(PerfOptions options)
        {
            _ = options;
            return 1;
        }
    }
}
