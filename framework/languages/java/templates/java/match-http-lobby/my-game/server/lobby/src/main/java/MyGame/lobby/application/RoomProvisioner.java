package MyGame.lobby.application;

import MyGame.MatchResponse;

import java.util.concurrent.CompletionStage;

public interface RoomProvisioner {
    CompletionStage<MatchResponse> prepare(String roomId);
}
