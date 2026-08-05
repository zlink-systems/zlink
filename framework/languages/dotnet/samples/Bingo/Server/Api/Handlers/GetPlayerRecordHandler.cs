using Bingo.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Handlers;

namespace Bingo.Server.Api.Handlers;

[ZLinkHandlerGroup("api")]
internal sealed class GetPlayerRecordHandler(
    BingoPlayerRecordStore records,
    ILogger<GetPlayerRecordHandler> logger)
    : IZLinkRequestHandler<GetPlayerRecordReq, GetPlayerRecordRes>
{
    public ValueTask<GetPlayerRecordRes> HandleAsync(
        GetPlayerRecordReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var record = records.Get(request.ActorId);
        logger.LogInformation(
            "api player record: loaded. actor={ActorId}, wins={Wins}, losses={Losses}",
            record.ActorId,
            record.Wins,
            record.Losses);
        return ValueTask.FromResult(record);
    }
}
