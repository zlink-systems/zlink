using Bingo.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Handlers;

namespace Bingo.Server.Api.Handlers;

[ZLinkHandlerGroup("api")]
internal sealed class ReportBingoResultHandler(
    BingoPlayerRecordStore records,
    ILogger<ReportBingoResultHandler> logger)
    : IZLinkRequestHandler<ReportBingoResultReq, ReportBingoResultRes>
{
    public ValueTask<ReportBingoResultRes> HandleAsync(
        ReportBingoResultReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var record = records.Report(request.ActorId, request.Won);
        logger.LogInformation(
            "api bingo result: reported. room={RoomId}, actor={ActorId}, won={Won}, finalDrawSeq={FinalDrawSeq}, wins={Wins}, losses={Losses}",
            request.RoomId,
            request.ActorId,
            request.Won,
            request.FinalDrawSeq,
            record.Wins,
            record.Losses);
        return ValueTask.FromResult(record);
    }
}
