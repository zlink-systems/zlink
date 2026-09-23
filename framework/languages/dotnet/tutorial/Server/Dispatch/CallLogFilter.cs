using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Handlers;

namespace Tutorial.Server.Dispatch;

// Runs around every handler this node receives, so the same logging is not
// repeated in each handler. Calling next() runs the handler; skipping it does
// not.
// --8<-- [start:filter-implementation]
public sealed class CallLogFilter(ILogger<CallLogFilter> logger) : IZLinkHandlerFilter
{
    public async ValueTask InvokeAsync(
        IZLinkHandlerFilterContext context,
        ZLinkHandlerFilterNext next,
        CancellationToken cancellationToken
    )
    {
        var startedAt = DateTimeOffset.UtcNow;
        logger.LogInformation("dispatch start: {Packet}", context.PacketName);

        await next();

        // Everything after next() runs on the way back out, so the filters
        // unwind in reverse registration order.
        var elapsed = DateTimeOffset.UtcNow - startedAt;
        logger.LogInformation(
            "dispatch done: {Packet} in {Elapsed}ms",
            context.PacketName,
            (int)elapsed.TotalMilliseconds
        );
    }
}
// --8<-- [end:filter-implementation]
