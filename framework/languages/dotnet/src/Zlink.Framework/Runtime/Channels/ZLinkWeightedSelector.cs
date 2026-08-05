namespace Zlink.Framework.Runtime.Channels;

internal static class ZLinkWeightedSelector
{
    // Object placement still uses a per-operation cursor. It is deliberately
    // kept separate from channel selection, whose current values belong to the
    // channel route and are supplied to the overload below.
    internal static T? Select<T>(
        IReadOnlyList<T> eligible,
        Func<T, int> weight,
        ref long cursor)
        where T : class
    {
        if (eligible.Count == 0)
            return null;
        var rawTotal = Sum(eligible, weight);
        if (rawTotal <= 0)
            return null;
        var divisor = CommonDivisor(eligible, weight);
        var total = rawTotal / divisor;
        var ordinal = Interlocked.Increment(ref cursor);
        var step = CoprimeStep(total);
        var selected = (long)(((ulong)ordinal * (ulong)step) % (ulong)total);
        foreach (var candidate in eligible)
        {
            var candidateWeight = weight(candidate) / divisor;
            if (candidateWeight <= 0)
                continue;
            if (selected < candidateWeight)
                return candidate;
            selected -= candidateWeight;
        }
        throw new InvalidOperationException(
            "Weighted selection did not select an eligible target.");
    }

    // Smooth weighted round-robin. The caller owns the current values because
    // they are part of the logical channel route, not a property of one call.
    // Candidates must expose the contract's stable identifier so equal current
    // values do not depend on dictionary or connection enumeration order.
    internal static T? Select<T, TKey>(
        IReadOnlyList<T> eligible,
        Func<T, int> weight,
        Func<T, TKey> key,
        IDictionary<TKey, long> currents,
        IComparer<TKey> comparer)
        where T : class
        where TKey : notnull
    {
        if (eligible.Count == 0)
            return null;

        var total = SumPositive(eligible, weight);
        if (total <= 0)
            return null;

        T? selected = null;
        TKey? selectedKey = default;
        var selectedCurrent = long.MinValue;
        foreach (var candidate in eligible)
        {
            var candidateWeight = weight(candidate);
            if (candidateWeight <= 0)
                continue;

            var candidateKey = key(candidate);
            var current = currents.TryGetValue(candidateKey, out var prior)
                ? checked(prior + candidateWeight)
                : candidateWeight;
            currents[candidateKey] = current;

            if (selected is null
                || current > selectedCurrent
                || current == selectedCurrent
                   && comparer.Compare(candidateKey, selectedKey!) < 0)
            {
                selected = candidate;
                selectedKey = candidateKey;
                selectedCurrent = current;
            }
        }

        if (selected is null)
            return null;

        currents[selectedKey!] = checked(selectedCurrent - total);
        return selected;
    }

    internal static long Sum<T>(
        IEnumerable<T> eligible,
        Func<T, int> weight) =>
        eligible.Aggregate(
            0L,
            (sum, candidate) => checked(sum + weight(candidate)));

    private static long SumPositive<T>(
        IEnumerable<T> eligible,
        Func<T, int> weight)
    {
        var total = 0L;
        foreach (var candidate in eligible)
        {
            var candidateWeight = weight(candidate);
            if (candidateWeight > 0)
                total = checked(total + candidateWeight);
        }
        return total;
    }

    private static int CommonDivisor<T>(
        IEnumerable<T> eligible,
        Func<T, int> weight)
    {
        var divisor = 0L;
        foreach (var candidate in eligible)
        {
            var candidateWeight = weight(candidate);
            if (candidateWeight <= 0)
                continue;
            divisor = divisor == 0
                ? candidateWeight
                : GreatestCommonDivisor(divisor, candidateWeight);
            if (divisor == 1)
                return 1;
        }
        return checked((int)divisor);
    }

    private static long CoprimeStep(long total)
    {
        if (total <= 2)
            return 1;
        var step = (total / 2) + 1;
        while (GreatestCommonDivisor(step, total) != 1)
            step++;
        return step;
    }

    private static long GreatestCommonDivisor(long left, long right)
    {
        while (right != 0)
        {
            var remainder = left % right;
            left = right;
            right = remainder;
        }
        return left;
    }
}
