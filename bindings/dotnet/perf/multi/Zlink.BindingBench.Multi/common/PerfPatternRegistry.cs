internal static class MultiPerfPatternRegistry
{
    private static readonly PerfPatternRegistry Registry = new(
        new IPerfPattern[]
        {
            new MultiPerfPattern("DEALER_DEALER",
                static options => PerfMultiDealerDealerServer.Run(options),
                static options => PerfMultiDealerDealerClient.Run(options)
                    .GetAwaiter().GetResult()),
            new MultiPerfPattern("DEALER_ROUTER",
                static options => PerfMultiDealerRouterServer.Run(options)
                    .GetAwaiter().GetResult(),
                static options => PerfMultiDealerRouterClient.Run(options)
                    .GetAwaiter().GetResult()),
            new MultiPerfPattern("DEALER_ROUTER_SENDSEND",
                static options => PerfMultiDealerRouterServer.Run(options)
                    .GetAwaiter().GetResult(),
                static options => PerfMultiDealerRouterClient.Run(options)
                    .GetAwaiter().GetResult()),
            new MultiPerfPattern("DEALER_ROUTER_REQREP",
                static options => PerfMultiSocketReqRep.RunDealerRouterServer(options),
                static options => PerfMultiSocketReqRep.RunDealerRouterClient(options)
                    .GetAwaiter().GetResult()),
            new MultiPerfPattern("ROUTER_ROUTER",
                static options => PerfMultiRouterRouterServer.Run(options)
                    .GetAwaiter().GetResult(),
                static options => PerfMultiRouterRouterClient.Run(options)
                    .GetAwaiter().GetResult()),
            new MultiPerfPattern("ROUTER_ROUTER_SENDSEND",
                static options => PerfMultiRouterRouterServer.Run(options)
                    .GetAwaiter().GetResult(),
                static options => PerfMultiRouterRouterClient.Run(options)
                    .GetAwaiter().GetResult()),
            new MultiPerfPattern("ROUTER_ROUTER_REQREP",
                static options => PerfMultiSocketReqRep.RunRouterRouterServer(options),
                static options => PerfMultiSocketReqRep.RunRouterRouterClient(options)
                    .GetAwaiter().GetResult()),
            new MultiPerfPattern("PUBSUB",
                static options => PerfMultiPubSubServer.Run(options),
                static options => PerfMultiPubSubClient.Run(options)),
            new MultiPerfPattern("STREAM",
                static options => PerfMultiStreamServer.Run(options),
                static options => PerfRunner.PrintExternalStreamClientError()),
        });

    internal static bool TryGet(string pattern, out IPerfPattern perfPattern)
    {
        return Registry.TryGet(pattern, out perfPattern);
    }

    private sealed class MultiPerfPattern : IPerfPattern
    {
        private readonly System.Func<PerfOptions, int> _runServer;
        private readonly System.Func<PerfOptions, int> _runClient;

        internal MultiPerfPattern(string name,
            System.Func<PerfOptions, int> runServer,
            System.Func<PerfOptions, int> runClient)
        {
            Name = name;
            _runServer = runServer;
            _runClient = runClient;
        }

        public string Name { get; }

        public int RunSingle(PerfOptions options)
        {
            _ = options;
            return 1;
        }

        public int RunMultiServer(PerfOptions options)
        {
            return _runServer(options);
        }

        public int RunMultiClient(PerfOptions options)
        {
            return _runClient(options);
        }
    }
}
