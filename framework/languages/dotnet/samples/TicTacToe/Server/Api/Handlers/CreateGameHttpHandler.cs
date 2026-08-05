using TicTacToe.Server.Configuration;
using TicTacToe.Shared.Contracts;
using Zlink.Framework.Contracts.Spots;

namespace TicTacToe.Server.Api.Handlers;

internal static class CreateGameHttpHandler
{
    public static async Task<IResult> HandleAsync(
        CreateGameHttpReq request,
        IZLinkSpotManager spots,
        SampleSettings settings,
        ILoggerFactory loggerFactory,
        CancellationToken cancellationToken)
    {
        var logger = loggerFactory.CreateLogger("Game.Api.CreateGame");
        var gameName = !string.IsNullOrWhiteSpace(request.GameName)
            ? request.GameName
            : SampleDefaults.GameName;
        logger.LogInformation("client -> api: create game requested. game={GameName}", gameName);
        // --8<-- [start:doc-create]
        var created = await spots
            .Create(SampleTypes.GameSpot)          // 이 stable type을 등록한 node가 후보가 된다.
            .InMesh(SampleNodes.Mesh)              // Spot을 만들 mesh를 고른다.
            .Request(new TicTacToeGameCreateReq(   // 새 Spot의 생성 callback에 전달할 최초 설정이다.
                gameName,
                SampleDefaults.RequiredLevel))
            .Async(cancellationToken);             // .NET의 비동기 완료 terminal이다.
        // --8<-- [end:doc-create]

        logger.LogInformation(
            "api: game Spot ready. roomId={RoomId}, state={State}, game={GameName}",
            created.Spot.SpotId,
            created.State,
            gameName);

        return Results.Ok(new CreateGameHttpRes(
            created.Spot.SpotId,
            settings.PlayEndpoints,
            settings.PlayNodes,
            gameName,
            SampleDefaults.RequiredLevel));
    }
}
