// SPDX-License-Identifier: LicenseRef-Zlink
#nullable enable
using System;

namespace Zlink.NetworkSync
{
    internal sealed class MatchClock
    {
        private const ulong RttHistoryUs = 30_000_000;
        private readonly (ulong At, ulong Rtt)[] _samples = new (ulong, ulong)[32];
        private readonly ulong[] _sortedRtts = new ulong[32];
        private int _nextSample;
        private int _sampleCount;
        private ulong _observedAt;
        private ulong _lastRtt;
        private decimal _tick;
        private decimal _marginVariation;
        private long? _lastMargin;

        internal bool HasSample => _sampleCount != 0;
        internal ulong TickUs { get; private set; }
        internal decimal DownlinkUs { get; private set; }
        internal decimal JitterUs { get; private set; }
        internal decimal LeadUs { get; private set; }

        internal decimal TickAt(ulong now) => _tick + (decimal)(now - _observedAt) / TickUs;

        internal void Observe(MatchTimelineAnchor anchor, ulong now, ulong rtt, InputFeedback? feedback)
        {
            var hadSample = HasSample;
            var expectedTick = hadSample ? TickAt(now) : 0;
            _samples[_nextSample] = (now, rtt);
            _nextSample = (_nextSample + 1) % _samples.Length;
            _sampleCount = Math.Min(_sampleCount + 1, _samples.Length);
            var count = 0;
            for (var i = 0; i < _sampleCount; i++)
            {
                if (now - _samples[i].At <= RttHistoryUs)
                    _sortedRtts[count++] = _samples[i].Rtt;
            }
            Array.Sort(_sortedRtts, 0, count);

            var lowerHalfCount = (count + 1) / 2;
            var median = ((decimal)_sortedRtts[(lowerHalfCount - 1) / 2] + _sortedRtts[lowerHalfCount / 2]) / 2;
            DownlinkUs = median / 2;
            var observedTick = anchor.Tick + (anchor.SinceTickUs + DownlinkUs) / TickUs;
            _tick = hadSample ? expectedTick + Clamp(observedTick - expectedTick, -1, 1) / 4 : observedTick;
            _observedAt = now;

            if (hadSample)
                JitterUs += (Math.Abs((decimal)rtt - _lastRtt) - JitterUs) / 4;
            _lastRtt = rtt;
            if (_lastMargin is null)
                LeadUs = DownlinkUs + JitterUs + TickUs;

            if (feedback?.Disposition is InputDisposition.Accepted or InputDisposition.Late)
            {
                var margin = feedback.ArrivalMarginUs;
                if (_lastMargin is { } previous)
                    _marginVariation += (Math.Abs((decimal)margin - previous) - _marginVariation) / 4;
                _lastMargin = margin;
                var desiredMargin = TickUs + Math.Max(JitterUs, _marginVariation);
                var adjustment = Clamp(desiredMargin - margin, -(decimal)TickUs, TickUs) / 4;
                LeadUs = Math.Max(TickUs, LeadUs + adjustment);
            }
        }

        internal void Reset(ulong tickUs = 0)
        {
            TickUs = tickUs;
            _nextSample = 0;
            _sampleCount = 0;
            _lastMargin = null;
            _marginVariation = 0;
            JitterUs = 0;
        }

        private static decimal Clamp(decimal value, decimal min, decimal max) =>
            Math.Min(Math.Max(value, min), max);
    }
}
