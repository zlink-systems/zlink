// Verifies TD-A1 Terminator Surface behavior.
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Workers;
using Zlink.HttpClient;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdA1TerminatorSurfaceScenario
{
    public static Task RunAsync(ExecutionTurnScenarioContext context)
    {
        // Request calls expose Async/Yield. Actor Join registers a deferred
        // intent with Defer and reports completion through the Actor callback.
        AssertAwaitedTerminators(typeof(IZLinkRequestCall));
        AssertDeferredJoinTerminators(typeof(IZLinkActorJoinSpotCall));
        AssertDeferredJoinTerminators(typeof(IZLinkActorJoinEntrySpotCall));
        AssertTerminators(typeof(IZLinkWorkerCall<>));
        AssertHttpServerTerminators(typeof(ZLinkHttpServerRequestBuilder));

        var methods = typeof(ZLinkHttpRequestBuilder).GetMethods()
            .Select(method => method.Name)
            .ToHashSet(StringComparer.Ordinal);
        ZlinkStreamAssert.Ensure(methods.Contains("Async"),
            "TD-A1 standalone HTTP request builder is missing Async.");
        ZlinkStreamAssert.Ensure(!methods.Contains("Submit") && !methods.Contains("Yield"),
            "TD-A1 standalone HTTP request builder exposes server-only terminators.");
        // Spec http-client 05 §5.2 uses Fetch<T> inside an I/O Worker, so it is
        // part of the builder contract, and the standalone client withholds Yield.
        return Task.CompletedTask;
    }

    private static void AssertHttpServerTerminators(Type type)
    {
        var methods = type.GetMethods().Select(method => method.Name).ToHashSet(StringComparer.Ordinal);
        ZlinkStreamAssert.Ensure(methods.Contains("Async"),
            $"TD-A1 {type.Name} is missing Async.");
        // Spec http-client 05 §5.1: the server builder returns the shared Spot
        // gate with Yield where the framework allows it; the standalone client
        // has no gate to return.
        ZlinkStreamAssert.Ensure(methods.Contains("Yield"),
            $"TD-A1 {type.Name} is missing Yield.");
        ZlinkStreamAssert.Ensure(!methods.Contains("Submit"),
            $"TD-A1 {type.Name} exposes Submit.");
    }

    private static void AssertAwaitedTerminators(Type type)
    {
        var methods = type.GetMethods().Select(method => method.Name).ToHashSet(StringComparer.Ordinal);
        foreach (var name in new[] { "Async", "Yield" })
            ZlinkStreamAssert.Ensure(methods.Contains(name), $"TD-A1 {type.Name} is missing {name}.");
        ZlinkStreamAssert.Ensure(!methods.Contains("Submit"),
            $"TD-A1 {type.Name} exposes the fire-and-forget Submit terminator.");

    }

    private static void AssertTerminators(Type type)
    {
        var methods = type.GetMethods().Select(method => method.Name).ToHashSet(StringComparer.Ordinal);
        foreach (var name in new[] { "Submit", "Async", "Yield" })
            ZlinkStreamAssert.Ensure(methods.Contains(name), $"TD-A1 {type.Name} is missing {name}.");

    }

    private static void AssertDeferredJoinTerminators(Type type)
    {
        var methods = type.GetMethods()
            .Concat(type.GetInterfaces().SelectMany(contract => contract.GetMethods()))
            .Select(method => method.Name)
            .ToHashSet(StringComparer.Ordinal);
        ZlinkStreamAssert.Ensure(methods.Contains("Defer"), $"TD-A1 {type.Name} is missing Defer.");
        foreach (var name in new[] { "Async", "Yield", "Submit", "Fetch" })
            ZlinkStreamAssert.Ensure(!methods.Contains(name), $"TD-A1 {type.Name} exposes {name}.");
    }
}
