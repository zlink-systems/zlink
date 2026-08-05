using System.Diagnostics;

namespace Zlink.Framework.Runtime.Channels;

/// <summary>
/// Keeps a deterministic smooth weighted round-robin sequence for one stable
/// candidate set. The sequence is prepared when the candidate set changes so
/// the send path only advances a cursor.
/// </summary>
internal sealed class ZLinkWeightedSelectionPlan<T, TKey>
    where T : class
    where TKey : notnull
{
    private const int MaximumPrecomputedSteps = 4096;
    private static readonly long MaximumPrecomputeTicks = Math.Max(
        1,
        Stopwatch.Frequency / 100);

    private readonly T[] _candidates;
    private readonly TKey[] _keys;
    private readonly int[] _weights;
    private readonly long[] _initialCurrents;
    private readonly IComparer<TKey> _keyComparer;
    private readonly IEqualityComparer<TKey> _keyEqualityComparer;
    private readonly long _totalWeight;
    private readonly int[]? _sequence;
    private readonly int _cycleStart;
    private readonly int _cycleLength;
    private readonly long[]? _fallbackCurrents;
    private long _cursor;

    internal ZLinkWeightedSelectionPlan(
        IReadOnlyList<T> candidates,
        Func<T, int> weight,
        Func<T, TKey> key,
        IReadOnlyDictionary<TKey, long>? retainedCurrents,
        IEqualityComparer<TKey> keyEqualityComparer,
        IComparer<TKey> keyComparer)
    {
        ArgumentNullException.ThrowIfNull(candidates);
        ArgumentNullException.ThrowIfNull(weight);
        ArgumentNullException.ThrowIfNull(key);
        ArgumentNullException.ThrowIfNull(keyEqualityComparer);
        ArgumentNullException.ThrowIfNull(keyComparer);

        _keyEqualityComparer = keyEqualityComparer;
        _keyComparer = keyComparer;
        _candidates = candidates.ToArray();
        _keys = new TKey[_candidates.Length];
        _weights = new int[_candidates.Length];
        _initialCurrents = new long[_candidates.Length];

        var keys = new HashSet<TKey>(_keyEqualityComparer);
        for (var index = 0; index < _candidates.Length; index++)
        {
            var candidate = _candidates[index]
                ?? throw new ArgumentException(
                    "A selection plan cannot contain a null candidate.",
                    nameof(candidates));
            var candidateKey = key(candidate);
            if (!keys.Add(candidateKey))
                throw new ArgumentException(
                    "A selection plan requires unique candidate keys.",
                    nameof(candidates));
            var candidateWeight = weight(candidate);
            if (candidateWeight <= 0)
                throw new ArgumentOutOfRangeException(
                    nameof(candidates),
                    "A selection plan requires positive candidate weights.");

            _keys[index] = candidateKey;
            _weights[index] = candidateWeight;
            if (retainedCurrents is not null
                && retainedCurrents.TryGetValue(
                    candidateKey,
                    out var retainedCurrent))
                _initialCurrents[index] = retainedCurrent;
            _totalWeight = checked(_totalWeight + candidateWeight);
        }

        if (_candidates.Length == 0)
            return;

        if (TryPrepareSequence(
                out var sequence,
                out var cycleStart,
                out var cycleLength))
        {
            _sequence = sequence;
            _cycleStart = cycleStart;
            _cycleLength = cycleLength;
        }
        else
        {
            // The fallback preserves the exact current state at construction
            // time. The exploratory state used during bounded preparation is
            // never exposed as an accepted selection.
            _fallbackCurrents = (long[])_initialCurrents.Clone();
        }
    }

    internal int Count => _candidates.Length;

    internal IReadOnlyList<T> Candidates => _candidates;

    internal T? Select()
    {
        if (_candidates.Length == 0)
            return null;

        if (_sequence is null)
        {
            var selected = SelectIndex(_fallbackCurrents!);
            Advance(_fallbackCurrents!, selected);
            return _candidates[selected];
        }

        var position = _cursor < _sequence.Length
            ? checked((int)_cursor)
            : checked(
                _cycleStart
                + ((_cursor - _cycleStart) % _cycleLength));
        _cursor++;
        return _candidates[_sequence[position]];
    }

    /// <summary>
    /// Captures the logical current values for retained candidates. This is
    /// used only while replacing a topology snapshot.
    /// </summary>
    internal IReadOnlyDictionary<TKey, long> CaptureCurrents()
    {
        var currents = new Dictionary<TKey, long>(_keyEqualityComparer);
        if (_candidates.Length == 0)
            return currents;

        if (_sequence is null)
        {
            for (var index = 0; index < _keys.Length; index++)
                currents[_keys[index]] = _fallbackCurrents![index];
            return currents;
        }

        var state = (long[])_initialCurrents.Clone();
        if (_cursor <= _cycleStart)
        {
            ApplySelections(state, checked((int)_cursor), 0);
        }
        else
        {
            ApplySelections(state, _cycleStart, 0);
            var remainder = checked(
                (int)((_cursor - _cycleStart) % _cycleLength));
            ApplySelections(state, remainder, _cycleStart);
        }

        for (var index = 0; index < _keys.Length; index++)
            currents[_keys[index]] = state[index];
        return currents;
    }

    private bool TryPrepareSequence(
        out int[] sequence,
        out int cycleStart,
        out int cycleLength)
    {
        var visited = new Dictionary<SelectionState, int>();
        var steps = new List<int>();
        var state = (long[])_initialCurrents.Clone();
        var startedAt = Stopwatch.GetTimestamp();

        for (var step = 0; step < MaximumPrecomputedSteps; step++)
        {
            if (Stopwatch.GetTimestamp() - startedAt
                > MaximumPrecomputeTicks)
            {
                sequence = [];
                cycleStart = 0;
                cycleLength = 0;
                return false;
            }

            var stateKey = new SelectionState(state);
            if (visited.TryGetValue(stateKey, out var previous))
            {
                sequence = steps.ToArray();
                cycleStart = previous;
                cycleLength = step - previous;
                return true;
            }
            visited.Add(stateKey, step);

            var selected = SelectIndex(state);
            steps.Add(selected);
            Advance(state, selected);
        }

        sequence = [];
        cycleStart = 0;
        cycleLength = 0;
        return false;
    }

    private int SelectIndex(long[] currents)
    {
        var selected = -1;
        var selectedCurrent = long.MinValue;
        for (var index = 0; index < currents.Length; index++)
        {
            var candidateCurrent = checked(
                currents[index] + _weights[index]);
            if (selected < 0
                || candidateCurrent > selectedCurrent
                || candidateCurrent == selectedCurrent
                   && _keyComparer.Compare(
                       _keys[index],
                       _keys[selected]) < 0)
            {
                selected = index;
                selectedCurrent = candidateCurrent;
            }
        }
        return selected;
    }

    private void Advance(long[] currents, int selected)
    {
        for (var index = 0; index < currents.Length; index++)
            currents[index] = checked(currents[index] + _weights[index]);
        currents[selected] = checked(currents[selected] - _totalWeight);
    }

    private void ApplySelections(
        long[] currents,
        int count,
        int sequenceOffset)
    {
        for (var index = 0; index < count; index++)
            Advance(currents, _sequence![sequenceOffset + index]);
    }

    private sealed class SelectionState : IEquatable<SelectionState>
    {
        private readonly long[] _values;
        private readonly int _hashCode;

        internal SelectionState(long[] values)
        {
            _values = (long[])values.Clone();
            var hash = 17;
            foreach (var value in _values)
                hash = unchecked(hash * 31 + value.GetHashCode());
            _hashCode = hash;
        }

        public bool Equals(SelectionState? other) =>
            other is not null
            && _values.AsSpan().SequenceEqual(other._values);

        public override bool Equals(object? obj) =>
            obj is SelectionState other && Equals(other);

        public override int GetHashCode() => _hashCode;
    }
}
