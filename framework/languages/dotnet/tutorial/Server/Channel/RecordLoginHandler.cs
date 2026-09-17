using Tutorial.Shared;
using Zlink.Framework.Contracts.Handlers;

namespace Tutorial.Server.Channel;

// --8<-- [start:channel-send-handler]
// Handles a one-way message. There is no return value, so the caller is already
// done by the time this runs and cannot observe a failure here.
public sealed class RecordLoginHandler : IZLinkSendHandler<RecordLogin>
{
    private readonly ILogger<RecordLoginHandler> _logger;

    public RecordLoginHandler(ILogger<RecordLoginHandler> logger) => _logger = logger;

    public ValueTask HandleAsync(
        RecordLogin message,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        _logger.LogInformation("login recorded: {PlayerId}", message.PlayerId);
        return ValueTask.CompletedTask;
    }
}
// --8<-- [end:channel-send-handler]
