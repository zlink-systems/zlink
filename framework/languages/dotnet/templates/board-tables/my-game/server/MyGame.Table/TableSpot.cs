using System.Security.Cryptography;
using System.Text.Json;
using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Table;

public sealed class TableSpot(
    IZLinkSpotContext context,
    NodeSettings settings,
    IZLinkActorClient actors,
    Func<int, int>? randomIndex = null
) : IZLinkSpot<PlayerActor>
{
    public IZLinkSpotContext Context { get; } = context;
    private TableState state = new();
    private readonly Func<int, int> randomIndexSource = randomIndex ?? RandomNumberGenerator.GetInt32;

    public void Configure()
    {
        Context.Handlers.AddActorPacket<ReconnectHandler, PlayerActor>(nameof(JoinTableRequest));
        Context.Handlers.AddActorPacket<ViewHandler, PlayerActor>(nameof(TableViewRequest));
        Context.Handlers.AddActorPacket<StartHandler, PlayerActor>(nameof(StartHandRequest));
        Context.Handlers.AddActorPacket<BetHandler, PlayerActor>(nameof(BetRequest));
        Context.Handlers.AddActorPacket<TableViewDelivery, PlayerActor>(nameof(TableView));
        Context.Handlers.AddActorPacket<HandDealtDelivery, PlayerActor>(nameof(HandDealt));
        Context.Handlers.AddActorPacket<HandResultDelivery, PlayerActor>(nameof(HandResult));
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken
    )
    {
        var join = request.Decode<JoinTableRequest>();
        if (join.PlayerId != actorId || join.TableId != Context.SpotId)
            throw new InvalidDataException("Table admission identity mismatch.");
        if (Find(actorId) is null)
        {
            if (state.Seats.Count == GameNames.SeatCount)
                return ValueTask.FromResult(ZLinkSpotActorJoinResult.Reject());
            var seat = Enumerable
                .Range(0, GameNames.SeatCount)
                .First(index => state.Seats.All(player => player.Seat != index));
            state.Seats.Add(new PlayerSeat { Seat = seat, PlayerId = actorId });
            state.Seats.Sort((left, right) => left.Seat.CompareTo(right.Seat));
        }
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());
    }

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken cancellationToken) =>
        BroadcastAsync(cancellationToken);

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    private PlayerSeat? Find(string id) => state.Seats.Find(seat => seat.PlayerId == id);

    private SeatView[] Seats() =>
        state
            .Seats.Select(player => new SeatView(
                player.Seat,
                player.PlayerId,
                player.Chips,
                player.Acted,
                player.Cards.Length
            ))
            .ToArray();

    public async ValueTask ViewAsync(PlayerActor actor, CancellationToken ct)
    {
        var seat = Find(actor.Context.ActorId);
        if (seat is null)
        {
            await RejectAsync(actor, Rejection.NotSeated);
            return;
        }
        ct.ThrowIfCancellationRequested();
        await actor.Context.BoundSession.Send(CreateView(seat, Seats())).Async();
        await actor.Context.BoundSession.Send(new HandDealt(state.HandNumber, seat.Cards)).Async();
    }

    private TableView CreateView(PlayerSeat player, SeatView[] seats) =>
        new(
            Context.SpotId,
            settings.Id,
            player.Seat,
            state.HandNumber,
            state.InHand,
            state.TurnSeat,
            state.Pot,
            seats
        );

    private async ValueTask BroadcastAsync(CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        var seats = Seats();
        foreach (var player in state.Seats)
            await actors.SendToActor(player.PlayerId, CreateView(player, seats)).Async();
    }

    private static ValueTask RejectAsync(PlayerActor actor, string reason) =>
        actor.Context.BoundSession.Send(new ActionRejected(reason, 0)).Async();

    public async ValueTask StartAsync(PlayerActor actor, CancellationToken ct)
    {
        var reason =
            Find(actor.Context.ActorId) is null ? Rejection.NotSeated
            : state.InHand ? Rejection.HandInProgress
            : state.Seats.Count < GameNames.MinimumPlayers ? Rejection.NotEnoughPlayers
            : !GameNames.TryAddContractIntegers(state.HandNumber, 1, out _)
                ? Rejection.IntegerOutOfRange
            : null;
        if (reason is not null)
        {
            await RejectAsync(actor, reason);
            return;
        }
        state.Deck = Enumerable.Range(0, GameNames.DeckSize).ToList();
        for (var index = state.Deck.Count - 1; index > 0; index--)
        {
            var other = randomIndexSource(index + 1);
            (state.Deck[index], state.Deck[other]) = (state.Deck[other], state.Deck[index]);
        }
        state.HandNumber++;
        state.InHand = true;
        state.TurnSeat = state.Seats[0].Seat;
        foreach (var player in state.Seats)
        {
            player.Acted = false;
            player.Cards = state.Deck.TakeLast(GameNames.CardsPerHand).ToArray();
            state.Deck.RemoveRange(
                state.Deck.Count - GameNames.CardsPerHand,
                GameNames.CardsPerHand
            );
            await actors
                .SendToActor(player.PlayerId, new HandDealt(state.HandNumber, player.Cards))
                .Async();
        }
        await BroadcastAsync(ct);
    }

    public async ValueTask BetAsync(PlayerActor actor, BetRequest bet, CancellationToken ct)
    {
        var player = Find(actor.Context.ActorId);
        var reason = player is null ? Rejection.NotSeated : null;
        if (
            reason is null
            && (
                bet.HandNumber > GameNames.MaxContractInteger
                || bet.Amount > GameNames.MaxContractInteger
            )
        )
            reason = Rejection.IntegerOutOfRange;
        if (reason is null && (!state.InHand || bet.HandNumber != state.HandNumber))
            reason = Rejection.NoHand;
        if (reason is null && state.TurnSeat != player!.Seat)
            reason = Rejection.NotYourTurn;
        if (reason is null && (bet.Amount < 0 || bet.Amount > player!.Chips))
            reason = Rejection.InvalidAmount;
        if (reason is not null)
        {
            await RejectAsync(actor, reason);
            return;
        }

        if (!GameNames.TryAddContractIntegers(state.Pot, bet.Amount, out var nextPot))
        {
            await RejectAsync(actor, Rejection.IntegerOutOfRange);
            return;
        }

        var nextPlayer = state.Seats.FirstOrDefault(seat =>
            seat.Cards.Length != 0 && seat.Seat != player!.Seat && !seat.Acted
        );
        SettlementPlan? settlement = null;
        if (
            nextPlayer is null
            && !TryCreateSettlement(player!, bet.Amount, nextPot, out settlement)
        )
        {
            await RejectAsync(actor, Rejection.IntegerOutOfRange);
            return;
        }

        player!.Chips -= bet.Amount;
        state.Pot = nextPot;
        player.Acted = true;
        state.TurnSeat = nextPlayer?.Seat ?? GameNames.NoSeat;
        await BroadcastAsync(ct);
        if (settlement is null)
            return;

        state.InHand = false;
        state.TurnSeat = GameNames.NoSeat;
        state.Pot = 0;
        foreach (var seat in state.Seats)
            seat.Cards = [];
        for (var index = 0; index < settlement.WinnerSeats.Length; index++)
            settlement.WinnerSeats[index].Chips = settlement.WinnerBalances[index];
        var result = new HandResult(
            state.HandNumber,
            nextPot,
            settlement.WinnerSeats.Select(seat => seat.Seat).ToArray(),
            settlement.Hands,
            Seats()
        );
        foreach (var seat in state.Seats)
            await actors.SendToActor(seat.PlayerId, result).Async();
        await BroadcastAsync(ct);
    }

    private bool TryCreateSettlement(
        PlayerSeat bettor,
        long betAmount,
        long pot,
        out SettlementPlan plan
    )
    {
        var hands = state.Seats.Where(seat => seat.Cards.Length != 0).ToArray();
        if (hands.Length == 0)
        {
            plan = null!;
            return false;
        }

        static int Score(PlayerSeat seat) =>
            seat.Cards.Sum(card => card % GameNames.RankCount + GameNames.LowestRank);
        var highest = hands.Max(Score);
        var winners = hands.Where(seat => Score(seat) == highest).ToArray();
        var share = pot / winners.Length;
        var remainder = pot % winners.Length;
        var winnerBalances = new long[winners.Length];
        for (var index = 0; index < winners.Length; index++)
        {
            var winner = winners[index];
            var balance = winner == bettor ? winner.Chips - betAmount : winner.Chips;
            if (!GameNames.TryAddContractIntegers(balance, share, out balance))
            {
                plan = null!;
                return false;
            }
            if (index == 0 && !GameNames.TryAddContractIntegers(balance, remainder, out balance))
            {
                plan = null!;
                return false;
            }
            winnerBalances[index] = balance;
        }

        plan = new SettlementPlan(
            winners,
            hands.Select(seat => new RevealedHand(seat.Seat, seat.PlayerId, seat.Cards.ToArray()))
                .ToArray(),
            winnerBalances
        );
        return true;
    }

    private sealed record SettlementPlan(
        PlayerSeat[] WinnerSeats,
        RevealedHand[] Hands,
        long[] WinnerBalances
    );

    public byte[] Capture() => JsonSerializer.SerializeToUtf8Bytes(state);

    public void Restore(ReadOnlyMemory<byte> payload) =>
        state =
            JsonSerializer.Deserialize<TableState>(payload.Span)
            ?? throw new InvalidDataException("Missing table state.");
}

public sealed class PlayerSeat
{
    public int Seat { get; set; }
    public string PlayerId { get; set; } = "";
    public long Chips { get; set; } = GameNames.InitialChips;
    public bool Acted { get; set; }
    public int[] Cards { get; set; } = [];
}

public sealed class TableState
{
    public List<PlayerSeat> Seats { get; set; } = [];
    public List<int> Deck { get; set; } = [];
    public long HandNumber { get; set; }
    public bool InHand { get; set; }
    public int TurnSeat { get; set; } = GameNames.NoSeat;
    public long Pot { get; set; }
}

public sealed class TableRelocation : IZLinkSpotRelocationAdapter<TableSpot>
{
    public ValueTask<byte[]> CaptureAsync(TableSpot spot, CancellationToken cancellationToken) =>
        ValueTask.FromResult(spot.Capture());

    public ValueTask RestoreAsync(
        TableSpot spot,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken
    )
    {
        spot.Restore(payload);
        return ValueTask.CompletedTask;
    }
}
