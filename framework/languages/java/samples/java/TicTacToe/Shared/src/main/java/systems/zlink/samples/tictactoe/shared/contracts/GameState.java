package systems.zlink.samples.tictactoe.shared.contracts;

import org.jspecify.annotations.Nullable;

public record GameState(
        String roomId,
        String board,
        String status,
        @Nullable String winner,
        String nextTurn,
        @Nullable String xActorId,
        @Nullable String oActorId,
        @Nullable String lastMoveActorId,
        @Nullable Integer lastMoveCell) {}
