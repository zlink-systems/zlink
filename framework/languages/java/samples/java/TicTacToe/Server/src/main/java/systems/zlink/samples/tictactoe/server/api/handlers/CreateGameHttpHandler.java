package systems.zlink.samples.tictactoe.server.api.handlers;

import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RestController;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.samples.tictactoe.server.configuration.ApiSettings;
import systems.zlink.samples.tictactoe.server.configuration.SampleNames;
import systems.zlink.samples.tictactoe.shared.contracts.CreateGameHttpReq;
import systems.zlink.samples.tictactoe.shared.contracts.CreateGameHttpRes;
import systems.zlink.samples.tictactoe.shared.contracts.PlayNodeInfo;

@RestController
public final class CreateGameHttpHandler {
    private final ZLinkSpotManager spots;
    private final ApiSettings settings;

    public CreateGameHttpHandler(ZLinkSpotManager spots, ApiSettings settings) {
        this.spots = spots;
        this.settings = settings;
    }

    @PostMapping("/games")
    public java.util.concurrent.CompletionStage<CreateGameHttpRes> handle(@RequestBody CreateGameHttpReq request) {
        String gameName = gameName(request);
        // --8<-- [start:doc-create]
        return spots.create("tictactoe.game")   // 이 stable type을 등록한 node가 후보가 된다.
            .inMesh(SampleNames.SpotMesh)       // Spot을 만들 mesh를 고른다.
                .timeout(SampleNames.RequestTimeout)
            .submit()                           // Java의 비동기 완료 terminal이다.
        // --8<-- [end:doc-create]
            .thenApply(created -> new CreateGameHttpRes(
                created.spot().spotId(),
                gameName,
                settings.playEndpoints(),
                settings.playEndpoints().stream().map(PlayNodeInfo::new).toList(),
                SampleNames.RequiredLevel));
    }

    private static String gameName(CreateGameHttpReq request) {
        return request.gameName() == null || request.gameName().isBlank()
            ? "tictactoe-game"
            : request.gameName();
    }

}
