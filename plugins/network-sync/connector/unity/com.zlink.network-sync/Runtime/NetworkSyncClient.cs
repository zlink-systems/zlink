// SPDX-License-Identifier: LicenseRef-Zlink
#nullable enable
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;

[assembly: InternalsVisibleTo("Zlink.NetworkSync.Tests")]

namespace Zlink.NetworkSync
{
    public readonly struct SnapshotPair<TState>
    {
        public ulong FromTick { get; }
        public ulong ToTick { get; }
        public TState From { get; }
        public TState To { get; }
        public double Ratio { get; }
        public bool IsHolding => FromTick == ToTick;

        internal SnapshotPair(ulong fromTick, TState from, ulong toTick, TState to, double ratio)
        {
            FromTick = fromTick;
            From = from;
            ToTick = toTick;
            To = to;
            Ratio = ratio;
        }
    }

    /// <summary>
    /// Owns the clock and snapshots for one joined Actor on a Manual-dispatch connection.
    /// Call from the same thread that pumps the connector.
    /// </summary>
    public sealed class NetworkSyncClient<TInput, TAction, TState> : IDisposable
    {
        private const ulong SampleLifetimeUs = 2_000_000;
        private const ulong IdleProbeIntervalUs = 500_000;
        private const ulong WarmupProbeIntervalUs = 250_000;
        private const int SnapshotCapacity = 128;

        private readonly Func<ulong> _now;
        private readonly decimal _snapshotIntervalUs;
        private readonly MatchClock _clock = new();
        private readonly List<NetworkSyncSnapshotV1<TState>> _snapshots = new(SnapshotCapacity);
        private readonly PendingSamples _samples = new();

        private IZlinkStreamConnector? _connector;
        private IDisposable? _pongHandler;
        private IDisposable? _snapshotHandler;
        private string? _matchId;
        private ulong? _epoch;
        private ulong _nextProbeAt;
        private ulong _nextProbeId;
        private ulong? _lastActionId;
        private ulong? _lastTargetTick;
        private ulong? _readyAt;
        private bool _disposed;

        /// <param name="snapshotInterval">The server's snapshot period. Defaults to 50 ms.</param>
        public NetworkSyncClient(TimeSpan? snapshotInterval = null)
            : this(
                () => (ulong)((decimal)Stopwatch.GetTimestamp() * 1_000_000 / Stopwatch.Frequency),
                snapshotInterval)
        {
        }

        internal NetworkSyncClient(Func<ulong> clock, TimeSpan? snapshotInterval = null)
        {
            _now = clock;
            var period = snapshotInterval ?? TimeSpan.FromMilliseconds(50);
            if (period <= TimeSpan.Zero)
                throw new ArgumentOutOfRangeException(nameof(snapshotInterval));
            _snapshotIntervalUs = period.Ticks / 10m;
        }

        /// <summary>Registers the two match push handlers on the application's connector.</summary>
        public void Attach(IZlinkStreamConnector connector, string matchId)
        {
            if (_disposed)
                throw new ObjectDisposedException(GetType().Name);
            if (_connector != null)
                throw new InvalidOperationException("This client is already attached.");
            if (connector is null)
                throw new ArgumentNullException(nameof(connector));
            if (string.IsNullOrEmpty(matchId))
                throw new ArgumentException("A match ID is required.", nameof(matchId));
            if (connector.Options.DispatchMode != ZlinkStreamDispatchMode.Manual ||
                connector.Options.PayloadCodec != null)
                throw new ArgumentException("Use Manual dispatch and the default JSON codec.", nameof(connector));

            _pongHandler = connector.On<NetworkSyncPongV1>(NetworkSyncPackets.Pong, OnPong);
            try
            {
                _snapshotHandler = connector.On<NetworkSyncSnapshotV1<TState>>(
                    NetworkSyncPackets.Snapshot, OnSnapshot);
            }
            finally
            {
                if (_snapshotHandler is null)
                {
                    _pongHandler.Dispose();
                    _pongHandler = null;
                }
            }

            _matchId = matchId;
            _connector = connector;
        }

        /// <summary>Call after connector dispatch and await any idle probe send.</summary>
        public ValueTask Update()
        {
            if (_connector is not { } connector)
                return default;

            var now = _now();
            if (now < _nextProbeAt)
                return default;

            var probe = _nextProbeId++;
            var ping = new NetworkSyncPingV1 { MatchId = _matchId!, ProbeId = probe, T1Us = now };
            var send = connector.Send(ping).PacketName(NetworkSyncPackets.Ping);
            _nextProbeAt = now + (_clock.HasSample ? IdleProbeIntervalUs : WarmupProbeIntervalUs);
            _samples.Remember(true, probe, now);
            return send.Async();
        }

        /// <summary>
        /// Returns true after connector send completes. Returns false while preparing or when
        /// the target tick has already been submitted. The caller may retain actions on false.
        /// </summary>
        public async ValueTask<bool> SendInputAsync(
            TInput input, IReadOnlyList<TAction>? actions = null, CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (_connector is not { } connector || _readyAt is null)
                return false;

            var now = _now();
            var lastSnapshotTick = _snapshots[_snapshots.Count - 1].Anchor.Tick;
            var target = Math.Max(
                (decimal)lastSnapshotTick + 1,
                decimal.Ceiling(_clock.TickAt(now) + _clock.LeadUs / _clock.TickUs));
            if (target > ulong.MaxValue || target <= _lastTargetTick)
                return false;

            var count = actions?.Count ?? 0;
            var firstActionId = count != 0 && _lastActionId is { } last ? checked(last + 1) : 0;
            var lastActionId = count != 0 ? checked(firstActionId + (ulong)(count - 1)) : _lastActionId;
            var entries = count == 0 ? Array.Empty<NetworkSyncAction<TAction>>() : new NetworkSyncAction<TAction>[count];
            for (var i = 0; i < entries.Length; i++)
                entries[i] = new NetworkSyncAction<TAction> { ActionId = firstActionId + (ulong)i, Payload = actions![i] };

            var sentAt = _now();
            var packet = new NetworkSyncInputV1<TInput, TAction>
            {
                MatchId = _matchId!,
                Epoch = _epoch!.Value,
                TargetTick = (ulong)target,
                T1Us = sentAt,
                LastSnapshotTick = lastSnapshotTick,
                Input = input,
                Actions = entries,
            };
            var send = connector.Send(packet).PacketName(NetworkSyncPackets.Input);
            _lastActionId = lastActionId;
            _lastTargetTick = packet.TargetTick;
            _nextProbeAt = sentAt + IdleProbeIntervalUs;
            _samples.Remember(false, packet.TargetTick, sentAt);
            await send.Async(cancellationToken).ConfigureAwait(false);
            return true;
        }

        /// <summary>Returns false while preparing or after the hold period expires.</summary>
        public bool TryGetInterpolation(out SnapshotPair<TState> pair)
        {
            pair = default;
            if (_readyAt is not { } readyAt)
                return false;

            var now = _now();
            var holdUs = 2 * _snapshotIntervalUs;
            var delayUs = holdUs + _clock.DownlinkUs + _clock.JitterUs;
            var tick = _clock.TickAt(now) - delayUs / _clock.TickUs;
            var latest = _snapshots[_snapshots.Count - 1];
            if (tick >= _snapshots[0].Anchor.Tick && tick <= latest.Anchor.Tick)
            {
                for (var i = _snapshots.Count - 2; i >= 0; i--)
                {
                    var from = _snapshots[i];
                    if (tick < from.Anchor.Tick)
                        continue;
                    var to = _snapshots[i + 1];
                    var ratio = (double)((tick - from.Anchor.Tick) / (to.Anchor.Tick - from.Anchor.Tick));
                    pair = new SnapshotPair<TState>(from.Anchor.Tick, from.State!, to.Anchor.Tick, to.State!, ratio);
                    return true;
                }
            }

            // For a late render target the hold starts when it crossed the newest snapshot,
            // even if the application skipped rendering that frame.
            var outsideUs = tick > latest.Anchor.Tick ? (tick - latest.Anchor.Tick) * _clock.TickUs : now - readyAt;
            if (outsideUs >= holdUs)
                return false;

            pair = new SnapshotPair<TState>(latest.Anchor.Tick, latest.State!, latest.Anchor.Tick, latest.State!, 0);
            return true;
        }

        public void Dispose()
        {
            if (_disposed)
                return;
            _disposed = true;
            _pongHandler?.Dispose();
            _snapshotHandler?.Dispose();
            _pongHandler = null;
            _snapshotHandler = null;
            _connector = null;
            _matchId = null;
            _readyAt = null;
            _snapshots.Clear();
            _samples.Clear();
            _clock.Reset();
        }

        private ValueTask OnPong(ZlinkStreamMessage<NetworkSyncPongV1> message, CancellationToken _)
        {
            var now = _now();
            var pong = message.Payload;
            if (pong != null && Matches(pong.Anchor) &&
                TryEcho(true, pong.ProbeId, pong.T1Us, pong.HoldUs, now, out var rtt))
            {
                EnterEpoch(pong.Anchor);
                _clock.Observe(pong.Anchor, now, rtt, null);
                MarkReady(now);
            }
            return default;
        }

        private ValueTask OnSnapshot(ZlinkStreamMessage<NetworkSyncSnapshotV1<TState>> message, CancellationToken _)
        {
            var now = _now();
            var snapshot = message.Payload;
            if (snapshot is null || !Matches(snapshot.Anchor))
                return default;

            EnterEpoch(snapshot.Anchor);
            if (_snapshots.Count == 0 || snapshot.Anchor.Tick > _snapshots[_snapshots.Count - 1].Anchor.Tick)
            {
                if (_snapshots.Count == SnapshotCapacity)
                    _snapshots.RemoveAt(0);
                _snapshots.Add(snapshot);
            }

            if (snapshot.InputFeedback is { } feedback &&
                TryEcho(false, feedback.TargetTick, feedback.T1Us, feedback.HoldUs, now, out var rtt))
                _clock.Observe(snapshot.Anchor, now, rtt, feedback);

            MarkReady(now);
            return default;
        }

        private bool Matches(MatchTimelineAnchor anchor) =>
            _connector != null && string.Equals(anchor.MatchId, _matchId, StringComparison.Ordinal) &&
            (_epoch is null || anchor.Epoch > _epoch ||
                (anchor.Epoch == _epoch && anchor.TickUs == _clock.TickUs));

        private void EnterEpoch(MatchTimelineAnchor anchor)
        {
            if (_epoch == anchor.Epoch)
                return;
            if (_epoch != null)
            {
                _samples.Clear();
                _snapshots.Clear();
                _nextProbeAt = 0;
                _lastActionId = null;
                _lastTargetTick = null;
                _readyAt = null;
            }
            _epoch = anchor.Epoch;
            _clock.Reset(anchor.TickUs);
        }

        private void MarkReady(ulong now)
        {
            if (_clock.HasSample && _snapshots.Count >= 2)
                _readyAt ??= now;
        }

        private bool TryEcho(bool ping, ulong id, ulong t1, ulong hold, ulong now, out ulong rtt)
        {
            rtt = 0;
            if (now < t1 || now - t1 >= SampleLifetimeUs ||
                !_samples.Consume(ping, id, t1) || now - t1 <= hold)
                return false;
            rtt = now - t1 - hold;
            return true;
        }

        private sealed class PendingSamples
        {
            private readonly (bool Ping, ulong Id, ulong T1)[] _entries = new (bool, ulong, ulong)[256];
            private ulong _next;
            private ulong _oldest;

            internal void Remember(bool ping, ulong id, ulong t1)
            {
                _entries[_next % (ulong)_entries.Length] = (ping, id, t1);
                _next++;
                _oldest = Math.Max(_oldest, _next > (ulong)_entries.Length ? _next - (ulong)_entries.Length : 0);
            }

            internal bool Consume(bool ping, ulong id, ulong t1)
            {
                for (var index = _next; index > _oldest;)
                {
                    index--;
                    var entry = _entries[index % (ulong)_entries.Length];
                    if (entry.T1 < t1)
                        break;
                    if (entry == (ping, id, t1))
                    {
                        // An echo for any earlier submission can no longer update the clock.
                        _oldest = index + 1;
                        return true;
                    }
                }
                return false;
            }

            internal void Clear() => _oldest = _next;
        }
    }
}
