#nullable enable
using System;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;

namespace Zlink.NetworkSync.FpsDemo
{
    internal sealed class DemoClient : IAsyncDisposable
    {
        private readonly IZlinkStreamConnector connector;
        private NetworkSyncClient<Point, Point, RoomView>? match;
        private Point[]? pendingShot;

        public string Status { get; private set; } = "Connecting";
        public string PlayerId { get; }

        public DemoClient(Uri endpoint, string playerId)
        {
            PlayerId = playerId;
            connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
            {
                Endpoint = endpoint,
                DispatchMode = ZlinkStreamDispatchMode.Manual,
            });
            connector.On<MatchFound>((message, _) =>
            {
                match = new NetworkSyncClient<Point, Point, RoomView>();
                match.Attach(connector, message.Payload.RoomId);
                Status = "Synchronizing";
                return default;
            });
            connector.OnDisconnected((_, _) =>
            {
                match?.Dispose();
                match = null;
                pendingShot = null;
                Status = "Disconnected";
                return default;
            });
            connector.OnErrorReceived((error, _) =>
            {
                Status = error.Message;
                return default;
            });
        }

        public async Task JoinAsync(CancellationToken ct)
        {
            await connector.Connect.Async(ct);
            await connector.Send(new QueueRequest(PlayerId, "duel")).Async(ct);
            Status = "Waiting for an opponent";
        }

        public async Task UpdateAsync(Point move, Point? shot, CancellationToken ct)
        {
            await connector.Dispatch.Async(ct);
            if (match is null)
                return;

            if (shot is { } aim)
                pendingShot = new[] { aim };
            await match.Update();
            if (await match.SendInputAsync(move, pendingShot, ct))
                pendingShot = null;
        }

        public bool TryGetInterpolation(out SnapshotPair<RoomView> pair)
        {
            pair = default;
            return match is not null && match.TryGetInterpolation(out pair);
        }

        public async ValueTask DisposeAsync()
        {
            match?.Dispose();
            await connector.DisposeAsync();
        }
    }

    internal sealed class QueueRequest
    {
        public QueueRequest(string playerId, string mode) => (PlayerId, Mode) = (playerId, mode);
        public string PlayerId { get; }
        public string Mode { get; }
    }

    internal sealed class MatchFound
    {
        public MatchFound(string roomId) => RoomId = roomId;
        public string RoomId { get; }
    }

    internal readonly struct Point
    {
        [JsonConstructor]
        public Point(double x, double y, double z) => (X, Y, Z) = (x, y, z);
        public double X { get; }
        public double Y { get; }
        public double Z { get; }
    }

    internal enum MatchPhase { Waiting, Playing, Finished }

    internal sealed class RoomView
    {
        public RoomView(PlayerView[] players, MatchPhase phase, string? winner) =>
            (Players, Phase, Winner) = (players, phase, winner);

        public PlayerView[] Players { get; }
        public MatchPhase Phase { get; }
        public string? Winner { get; }
    }

    internal sealed class PlayerView
    {
        public PlayerView(string playerId, Point position, int hp) =>
            (PlayerId, Position, Hp) = (playerId, position, hp);

        public string PlayerId { get; }
        public Point Position { get; }
        public int Hp { get; }
    }
}
