using Bingo.Shared.Contracts;

namespace Bingo.Server.Play.Domain.Bingo;

internal sealed class BingoRoomGame(string roomId, BingoRoomSettings settings)
{
    private readonly List<BingoRoomPlayer> _players = [];
    private BingoGame? _game;
    private BingoRoomSettings _settings = settings;

    public string Status { get; private set; } = BingoRoomStatus.WaitingForPlayers;

    public bool IsReadyToDraw => _game?.IsReadyToDraw == true
                                 && Status == BingoRoomStatus.Running;

    public bool CanAcceptPlayer()
    {
        return Status == BingoRoomStatus.WaitingForPlayers
               && _players.Count < _settings.RequiredPlayers;
    }

    public void ApplySettings(BingoRoomSettings newSettings)
    {
        if (newSettings.RequiredPlayers <= 0)
            throw new InvalidOperationException("Bingo room requires at least one player.");

        if (newSettings.MaxDrawNumber <= 0)
            throw new InvalidOperationException("Bingo room requires at least one draw number.");

        _settings = newSettings;
        _game = null;
    }

    public BingoGameChange JoinPlayer(
        string actorId,
        string displayName,
        int wins,
        int losses)
    {
        var existing = _players.FirstOrDefault(player => player.ActorId == actorId);
        if (existing is not null) return new BingoGameChange(Snapshot(), []);

        if (Status != BingoRoomStatus.WaitingForPlayers || _players.Count >= _settings.RequiredPlayers)
            throw new InvalidOperationException($"Room {roomId} cannot accept more players.");

        var player = new BingoRoomPlayer(actorId, displayName, _players.Count, wins, losses);
        _players.Add(player);

        var events = new List<BingoGameEvent>();
        var joinedState = Snapshot();
        events.AddRange(PlayerJoinedEvents(player, joinedState));

        if (_players.Count == _settings.RequiredPlayers)
        {
            Status = BingoRoomStatus.Running;
            _game = new BingoGame(_settings, _players.Select(static roomPlayer => roomPlayer.ActorId).ToArray());
            events.AddRange(EventsForExistingPlayers(
                BingoRoomEventKind.GameStarted,
                Snapshot(),
                player.ActorId));
        }

        return new BingoGameChange(Snapshot(), events);
    }

    public BingoGameChange SubmitCard(string actorId, BingoCard card)
    {
        if (Status != BingoRoomStatus.Running)
            throw new InvalidOperationException($"Room is not accepting cards. status={Status}");

        RequireGame().SubmitCard(actorId, card);
        return new BingoGameChange(Snapshot(), []);
    }

    public BingoGameChange DrawNextNumber()
    {
        if (Status != BingoRoomStatus.Running) return new BingoGameChange(Snapshot(), [], true);

        var result = RequireGame().DrawNextNumber();
        if (result.IsFinished) Status = BingoRoomStatus.Finished;

        var state = Snapshot();
        var events = new List<BingoGameEvent>();
        if (result.Number is { } number) events.AddRange(NumberDrawnEvents(state, number));

        if (Status == BingoRoomStatus.Finished) events.AddRange(EventsForAll(BingoRoomEventKind.GameEnded, state));

        return new BingoGameChange(
            state,
            events,
            Status == BingoRoomStatus.Finished);
    }

    public BingoRoomState Snapshot()
    {
        var hostActorId = _players.Count == 0 ? string.Empty : _players[0].ActorId;
        var game = _game?.Snapshot() ?? new BingoGameSnapshot(0, null, [], []);
        var state = new BingoRoomState
        {
            RoomId = roomId,
            Status = Status,
            HostActorId = hostActorId,
            CanStart = Status == BingoRoomStatus.WaitingForPlayers && _players.Count == _settings.RequiredPlayers,
            DrawSeq = game.DrawSeq,
            DrawnNumbers = { game.DrawnNumbers },
            Players = { _players.Select(player => player.ToState(hostActorId, _game)) },
            Winners = { game.Winners }
        };
        if (game.LastDrawnNumber is { } lastDrawnNumber) state.LastDrawnNumber = lastDrawnNumber;

        return state;
    }

    public BingoRoomState PreviewPlayerJoins(IEnumerable<BingoRoomJoinReq> pendingJoins)
    {
        var state = Snapshot();
        foreach (var request in pendingJoins)
        {
            if (state.Players.Any(player => string.Equals(player.ActorId, request.ActorId, StringComparison.Ordinal)))
                continue;
            if (state.Players.Count == _settings.RequiredPlayers) break;

            state.Players.Add(new BingoPlayerState
            {
                ActorId = request.ActorId,
                DisplayName = request.DisplayName,
                Seat = state.Players.Count,
                IsHost = state.Players.Count == 0
            });
        }

        if (state.Players.Count == _settings.RequiredPlayers)
            state.Status = BingoRoomStatus.Running;
        state.HostActorId = state.Players.OrderBy(static player => player.Seat).FirstOrDefault()?.ActorId
                            ?? string.Empty;
        state.CanStart = false;
        return state;
    }

    public static BingoRoomGame Restore(
        string roomId,
        BingoRoomSettings settings,
        BingoRoomState state)
    {
        var restored = new BingoRoomGame(roomId, settings);
        foreach (var player in state.Players.OrderBy(static player => player.Seat))
            restored.JoinPlayer(
                player.ActorId,
                player.DisplayName,
                player.Wins,
                player.Losses);

        if (state.Status is BingoRoomStatus.Running or BingoRoomStatus.Finished)
        {
            foreach (var player in state.Players.Where(static player => player.Card.Count > 0))
                restored.SubmitCard(
                    player.ActorId,
                    BingoCard.FromSubmittedNumbers(player.Card));
            for (var i = 0; i < state.DrawSeq; i++)
                restored.DrawNextNumber();
        }
        return restored;
    }

    private IReadOnlyList<BingoGameEvent> PlayerJoinedEvents(
        BingoRoomPlayer joined,
        BingoRoomState state)
    {
        return _players
            .Where(player => !string.Equals(player.ActorId, joined.ActorId, StringComparison.Ordinal))
            .Select(player => new BingoGameEvent(
                BingoRoomEventKind.PlayerJoined,
                player.ActorId,
                state,
                joined.ActorId,
                joined.DisplayName,
                joined.Seat,
                joined.Seat == 0))
            .ToArray();
    }

    private IReadOnlyList<BingoGameEvent> NumberDrawnEvents(
        BingoRoomState state,
        int number)
    {
        return _players
            .Select(player => new BingoGameEvent(
                BingoRoomEventKind.NumberDrawn,
                player.ActorId,
                state,
                DrawnNumber: number))
            .ToArray();
    }

    private IReadOnlyList<BingoGameEvent> EventsForAll(
        BingoRoomEventKind kind,
        BingoRoomState state)
    {
        return _players
            .Select(player => new BingoGameEvent(kind, player.ActorId, state))
            .ToArray();
    }

    private IReadOnlyList<BingoGameEvent> EventsForExistingPlayers(
        BingoRoomEventKind kind,
        BingoRoomState state,
        string joinedActorId)
    {
        return _players
            .Where(player => !string.Equals(player.ActorId, joinedActorId, StringComparison.Ordinal))
            .Select(player => new BingoGameEvent(kind, player.ActorId, state))
            .ToArray();
    }

    private BingoGame RequireGame()
    {
        return _game ?? throw new InvalidOperationException("Bingo game has not started.");
    }

    private sealed record BingoRoomPlayer(
        string ActorId,
        string DisplayName,
        int Seat,
        int Wins,
        int Losses)
    {
        public BingoPlayerState ToState(string hostActorId, BingoGame? game)
        {
            var card = game?.CardSnapshot(ActorId) ?? BingoCardSnapshot.Empty;
            return new BingoPlayerState
            {
                ActorId = ActorId,
                DisplayName = DisplayName,
                Seat = Seat,
                IsHost = string.Equals(ActorId, hostActorId, StringComparison.Ordinal),
                Card = { card.Numbers },
                Marks = { card.Marks },
                CompletedLines = card.CompletedLines,
                Wins = Wins,
                Losses = Losses
            };
        }
    }
}
