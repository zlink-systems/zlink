using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.UnitTests;

internal static class MeshDispatchTestSupport
{
    internal static void SetNodeRouteHandler(
        this ZLinkMeshDispatchPump pump,
        Func<IReadOnlyList<ZLinkBackendRouteReceived>, CancellationToken, ValueTask> handler,
        ZLinkRuntimeTaskRunner? runner = null
    ) =>
        pump.SetNodeRouteHandler(
            (records, pending, token) =>
            {
                var invocation = handler(records, token);
                if (invocation.IsCompletedSuccessfully)
                    invocation.GetAwaiter().GetResult();
                else
                    pending.Add(invocation.AsTask());
            },
            runner
        );

    internal static async ValueTask DispatchBatchAsync(
        this ZLinkMeshNodeRouteDispatcher dispatcher,
        IReadOnlyList<ZLinkBackendRouteReceived> batch,
        CancellationToken cancellationToken
    )
    {
        var pending = new List<Task>();
        dispatcher.DispatchBatch(batch, pending, cancellationToken);
        if (pending.Count != 0)
            await Task.WhenAll(pending).ConfigureAwait(false);
    }
}
