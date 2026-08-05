using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;
using ZoneWorld.Server.Ops.Application.Ops;
using ZoneWorld.Server.Ops.Infrastructure.ZLink.Handlers;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.Ops.Infrastructure.ZLink.Sessions;

/// <summary>
/// One operator console. Node state reaches it by push only — it never polls (§10.2), so
/// a node going down shows up as a state change rather than a failed request.
/// </summary>
public sealed class OpsConsoleSession(
    IZLinkSessionContext context,
    OpsConsoleRegistry consoles,
    ILogger<OpsConsoleSession> logger) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
    {
        consoles.Add(Context);
        logger.LogInformation("ops console connected. session={SessionId}", Context.SessionId);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
    {
        consoles.Remove(Context);
        logger.LogInformation("ops console disconnected. session={SessionId}", Context.SessionId);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken)
    {
        logger.LogWarning(
            "ops stream error. session={SessionId}, code={Code}, diagnostic={Diagnostic}",
            Context.SessionId,
            error.Error,
            error.Message);
        return ValueTask.CompletedTask;
    }

    /// <summary>The handler group answers every packet the console sends, and whether one was
    /// found is the framework's business, not this session's.</summary>
    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken) =>
        await Context.Handlers.TryHandleAsync(dispatch, payload, cancellationToken);
}
