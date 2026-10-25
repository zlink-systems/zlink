using System.Reflection;
using System.Text.Json;
using MyGame.Shared;
using MyGame.Table;
using Systems.Zlink;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Contracts.Timers;
using Zlink.Framework.Contracts.Workers;

namespace MyGame.E2E;

internal static class DomainChecks
{
    private static readonly Func<Task>[] Checks =
    [
        CheckDeckUniquenessAsync,
        CheckInjectedShuffleOrderAsync,
        CheckSettlementClearsCardsAsync,
        CheckBetIntegerBoundaryAsync,
        CheckPotOverflowAsync,
        CheckSettlementOverflowAsync,
        CheckHandNumberBoundaryAsync,
    ];

    public static async Task RunAsync()
    {
        var passed = 0;
        var failures = new List<Exception>();
        for (var index = 0; index < Checks.Length; index++)
        {
            try
            {
                await Checks[index]();
                passed++;
                Console.WriteLine($"✓ domain check {index + 1}");
            }
            catch (Exception error)
            {
                failures.Add(error);
                Console.Error.WriteLine($"✗ domain check {index + 1}: {error.Message}");
            }
        }

        Console.WriteLine($"Domain checks: {passed}/{Checks.Length}");
        if (failures.Count != 0)
            throw new AggregateException("Domain checks failed.", failures);
    }

    private static async Task CheckDeckUniquenessAsync()
    {
        var fixture = Fixture.Create();
        fixture.Restore(WaitingTable());
        await fixture.Spot.StartAsync(fixture.P1, CancellationToken.None);

        var state = fixture.State;
        var cards = state.Deck.Concat(state.Seats.SelectMany(seat => seat.Cards)).ToArray();
        Require(
            cards.Length == GameNames.DeckSize
                && cards.Distinct().Count() == GameNames.DeckSize
                && cards.Order().SequenceEqual(Enumerable.Range(0, GameNames.DeckSize)),
            "The dealt hands and remaining deck do not contain each card exactly once."
        );
    }

    private static async Task CheckInjectedShuffleOrderAsync()
    {
        var randomCalls = 0;
        var fixture = Fixture.Create(maximum =>
        {
            randomCalls++;
            return maximum - 1;
        });
        fixture.Restore(WaitingTable());
        await fixture.Spot.StartAsync(fixture.P1, CancellationToken.None);

        var state = fixture.State;
        Require(
            randomCalls == GameNames.DeckSize - 1,
            "The injected test RNG was not used for every shuffle step."
        );
        Require(
            state.Deck.SequenceEqual(Enumerable.Range(0, GameNames.DeckSize - 4))
                && state.Seats[0].Cards.SequenceEqual(new[] { 50, 51 })
                && state.Seats[1].Cards.SequenceEqual(new[] { 48, 49 }),
            "The injected no-swap RNG did not produce the expected card order."
        );
    }

    private static async Task CheckSettlementClearsCardsAsync()
    {
        var fixture = Fixture.Create();
        fixture.Restore(WaitingTable());
        await fixture.Spot.StartAsync(fixture.P1, CancellationToken.None);
        await fixture.Spot.BetAsync(fixture.P1, new BetRequest(1, 0), CancellationToken.None);
        await fixture.Spot.BetAsync(fixture.P2, new BetRequest(1, 0), CancellationToken.None);

        var result = fixture.Actors.Messages.OfType<HandResult>().LastOrDefault();
        var state = fixture.State;
        await fixture.Spot.ViewAsync(fixture.P1, CancellationToken.None);
        var privateHand = fixture.P1Session.Messages.OfType<HandDealt>().LastOrDefault();
        Require(
            result is not null
                && result.Seats.All(seat => seat.CardCount == 0)
                && state.Seats.All(seat => seat.Cards.Length == 0)
                && privateHand is not null
                && privateHand.Cards.Length == 0,
            "Settlement or the subsequent private-hand query retained cards."
        );
    }

    private static async Task CheckBetIntegerBoundaryAsync()
    {
        Require(
            GameNames.IsContractInteger(0)
                && GameNames.IsContractInteger(GameNames.MaxContractInteger)
                && !GameNames.IsContractInteger(-1)
                && !GameNames.IsContractInteger(GameNames.MaxContractInteger + 1)
                && GameNames.TryAddContractIntegers(
                    GameNames.MaxContractInteger - 1,
                    1,
                    out var maximum
                )
                && maximum == GameNames.MaxContractInteger
                && !GameNames.TryAddContractIntegers(GameNames.MaxContractInteger, 1, out _),
            "Contract integer boundary helpers returned an unexpected result."
        );
        Require(
            typeof(ActionRejected).GetProperty(nameof(ActionRejected.RetryAfterMs))?.PropertyType
                == typeof(int),
            "RetryAfterMs must use a 32-bit integer."
        );
        var encoded = new ActionRejected(Rejection.IntegerOutOfRange, 0).ToJson();
        using (var document = JsonDocument.Parse(encoded.Payload))
            Require(
                document.RootElement.TryGetProperty("retryAfterMs", out var retryAfter)
                    && retryAfter.GetInt32() == 0
                    && !document.RootElement.TryGetProperty("RetryAfterMs", out _),
                "Typed JSON must encode RetryAfterMs as retryAfterMs."
            );

        var accepted = Fixture.Create();
        accepted.Restore(InHandTable(p1Chips: GameNames.MaxContractInteger));
        await accepted.Spot.BetAsync(
            accepted.P1,
            new BetRequest(1, GameNames.MaxContractInteger),
            CancellationToken.None
        );
        var acceptedState = accepted.State;
        Require(
            !accepted.P1Session.Messages.OfType<ActionRejected>().Any()
                && acceptedState.Seats[0].Chips == 0
                && acceptedState.Seats[1].Chips == GameNames.InitialChips
                && acceptedState.Pot == GameNames.MaxContractInteger
                && acceptedState.TurnSeat == 1
                && GameNames.TryAddContractIntegers(
                    acceptedState.Seats[0].Chips,
                    acceptedState.Pot,
                    out var conserved
                )
                && conserved == GameNames.MaxContractInteger
                && acceptedState.Seats.Sum(seat => seat.Chips) + acceptedState.Pot
                    == GameNames.MaxContractInteger + GameNames.InitialChips,
            "A bet at the maximum contract integer was not accepted atomically."
        );

        var rejected = Fixture.Create();
        rejected.Restore(InHandTable(p1Chips: GameNames.MaxContractInteger));
        var before = rejected.Spot.Capture();
        await rejected.Spot.BetAsync(
            rejected.P1,
            new BetRequest(1, GameNames.MaxContractInteger + 1),
            CancellationToken.None
        );
        Require(
            LastRejection(rejected.P1Session)?.Reason == Rejection.IntegerOutOfRange
                && before.SequenceEqual(rejected.Spot.Capture()),
            "A bet above the maximum contract integer was not rejected without changing state."
        );
    }

    private static async Task CheckPotOverflowAsync()
    {
        var fixture = Fixture.Create();
        fixture.Restore(InHandTable(p1Chips: 1, pot: GameNames.MaxContractInteger));
        var before = fixture.Spot.Capture();
        await fixture.Spot.BetAsync(fixture.P1, new BetRequest(1, 1), CancellationToken.None);

        Require(
            LastRejection(fixture.P1Session)?.Reason == Rejection.IntegerOutOfRange
                && before.SequenceEqual(fixture.Spot.Capture()),
            "A pot sum above the maximum contract integer was not rejected without changing state."
        );
    }

    private static async Task CheckSettlementOverflowAsync()
    {
        var fixture = Fixture.Create();
        fixture.Restore(InHandTable(p1Chips: GameNames.MaxContractInteger));
        await fixture.Spot.BetAsync(fixture.P1, new BetRequest(1, 0), CancellationToken.None);
        var before = fixture.Spot.Capture();
        await fixture.Spot.BetAsync(fixture.P2, new BetRequest(1, 1), CancellationToken.None);

        Require(
            LastRejection(fixture.P2Session)?.Reason == Rejection.IntegerOutOfRange
                && before.SequenceEqual(fixture.Spot.Capture()),
            "An overflowing settlement payout was not rejected without changing state."
        );
    }

    private static async Task CheckHandNumberBoundaryAsync()
    {
        var fixture = Fixture.Create();
        fixture.Restore(WaitingTable(handNumber: GameNames.MaxContractInteger));
        var before = fixture.Spot.Capture();
        await fixture.Spot.StartAsync(fixture.P1, CancellationToken.None);

        Require(
            LastRejection(fixture.P1Session)?.Reason == Rejection.IntegerOutOfRange
                && before.SequenceEqual(fixture.Spot.Capture()),
            "Starting a hand beyond the maximum hand number was not rejected without changing state."
        );
    }

    private static ActionRejected? LastRejection(RecordingBoundSession session) =>
        session.Messages.OfType<ActionRejected>().LastOrDefault();

    private static TableState WaitingTable(long handNumber = 0) =>
        new()
        {
            Seats = [Seat(0, "p1"), Seat(1, "p2")],
            Deck = [],
            HandNumber = handNumber,
            InHand = false,
            TurnSeat = GameNames.NoSeat,
            Pot = 0,
        };

    private static TableState InHandTable(
        long p1Chips = GameNames.InitialChips,
        long p2Chips = GameNames.InitialChips,
        long pot = 0
    ) =>
        new()
        {
            Seats =
            [
                Seat(0, "p1", p1Chips, [11, 24]),
                Seat(1, "p2", p2Chips, [0, 13]),
            ],
            Deck = [],
            HandNumber = 1,
            InHand = true,
            TurnSeat = 0,
            Pot = pot,
        };

    private static PlayerSeat Seat(
        int seat,
        string playerId,
        long chips = GameNames.InitialChips,
        int[]? cards = null
    ) =>
        new()
        {
            Seat = seat,
            PlayerId = playerId,
            Chips = chips,
            Cards = cards ?? [],
        };

    private static void Require(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }

    private sealed class Fixture
    {
        private Fixture(
            RecordingActorClient actors,
            RecordingBoundSession p1Session,
            RecordingBoundSession p2Session,
            TableSpot spot,
            PlayerActor p1,
            PlayerActor p2
        )
        {
            Actors = actors;
            P1Session = p1Session;
            P2Session = p2Session;
            Spot = spot;
            P1 = p1;
            P2 = p2;
        }

        public RecordingActorClient Actors { get; }
        public RecordingBoundSession P1Session { get; }
        public RecordingBoundSession P2Session { get; }
        public TableSpot Spot { get; }
        public PlayerActor P1 { get; }
        public PlayerActor P2 { get; }

        public TableState State =>
            JsonSerializer.Deserialize<TableState>(Spot.Capture())
            ?? throw new InvalidDataException("Table state could not be captured.");

        public static Fixture Create(Func<int, int>? randomIndex = null)
        {
            var actors = new RecordingActorClient();
            var p1Session = new RecordingBoundSession();
            var p2Session = new RecordingBoundSession();
            var settings = new NodeSettings("test-node", "redis", "test", "mesh");
            var spot = new TableSpot(
                new TestSpotContext("test-table"),
                settings,
                actors,
                randomIndex
            );
            var p1 = new PlayerActor(new TestActorContext("p1", p1Session));
            var p2 = new PlayerActor(new TestActorContext("p2", p2Session));
            return new Fixture(actors, p1Session, p2Session, spot, p1, p2);
        }

        public void Restore(TableState state) => Spot.Restore(JsonSerializer.SerializeToUtf8Bytes(state));
    }

    private sealed class RecordingActorClient : IZLinkActorClient
    {
        public List<object> Messages { get; } = [];

        public IZLinkActorSendCall SendToActor<TMessage>(string actorId, TMessage message)
        {
            Messages.Add(message!);
            return DispatchProxy.Create<IZLinkActorSendCall, NoOpCallProxy>();
        }

        public IZLinkActorRequestCall RequestToActor<TRequest>(string actorId, TRequest request) =>
            throw new NotSupportedException();
    }

    private sealed class RecordingBoundSession : IZLinkBoundSession
    {
        public List<object> Messages { get; } = [];

        public IZLinkBoundSessionSendCall Send<TMessage>(TMessage message)
        {
            Messages.Add(message!);
            return DispatchProxy.Create<IZLinkBoundSessionSendCall, NoOpCallProxy>();
        }

        public ValueTask DisconnectAsync(CancellationToken cancellationToken = default) =>
            ValueTask.CompletedTask;
    }

    private class NoOpCallProxy : DispatchProxy
    {
        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args) =>
            targetMethod?.Name switch
            {
                "Metadata" => this,
                "Async" => ValueTask.CompletedTask,
                _ => throw new NotSupportedException($"Unexpected call: {targetMethod?.Name}."),
            };
    }

    private sealed class TestActorContext(string actorId, IZLinkBoundSession boundSession)
        : IZLinkActorContext
    {
        public string ActorId { get; } = actorId;
        public ulong ObjectGeneration => 1;
        public string MeshName => "test-mesh";
        public string? SpotId => "test-table";
        public IZLinkBoundSession BoundSession { get; } = boundSession;

        public IZLinkActorJoinSpotCall JoinSpot(string spotId, ZLinkMessage request) =>
            throw new NotSupportedException();

        public IZLinkActorJoinEntrySpotCall JoinEntrySpot(ZLinkMessage request) =>
            throw new NotSupportedException();
    }

    private sealed class TestSpotContext(string spotId) : IZLinkSpotContext
    {
        public string MeshName => "test-mesh";
        public string SpotId { get; } = spotId;
        public ulong ObjectGeneration => 1;
        public RoutingId NodeRid => default;
        public IZLinkSpotOutbound Outbound => null!;
        public IZLinkSpotHandlerRegistry Handlers => null!;

        public ValueTask<IZLinkTimer> AddTimer<THandler>(
            string name,
            TimeSpan period,
            ZLinkTimerOptions? options = null,
            CancellationToken cancellationToken = default
        )
            where THandler : class => throw new NotSupportedException();

        public IZLinkWorkerCall<TResult> RunCpuWorker<TResult>(Func<CancellationToken, TResult> work) =>
            throw new NotSupportedException();

        public IZLinkWorkerCall<TResult> RunIoWorker<TResult>(
            Func<CancellationToken, ValueTask<TResult>> work
        ) => throw new NotSupportedException();

        public IZLinkSpotRelocationReadyCall RelocationReady() => throw new NotSupportedException();

        public ValueTask LeaveActorAsync(
            IZLinkActor actor,
            CancellationToken cancellationToken = default
        ) => throw new NotSupportedException();

        public ValueTask<bool> CloseAsync(CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();
    }
}
