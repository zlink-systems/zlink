using System.Collections.Concurrent;

namespace Zlink.Framework.Contracts.Codecs;

internal sealed class ZLinkSerializerSelectionRegistry
{
    internal const int MaximumCachedDeclaredTypes = 1_024;

    private readonly Dictionary<string, Registration> _registrations =
        new(StringComparer.Ordinal);
    private readonly List<string> _registrationOrder = [];
    private readonly ConcurrentDictionary<Type, Resolution> _resolvedByDeclaredType = new();
    private readonly object _cacheGate = new();
    private int _frozen;
    private (string ContentType, IZLinkMessageSerializer Serializer)? _fallback;

    internal IReadOnlyDictionary<string, IZLinkMessageSerializer> Serializers =>
        _registrations.ToDictionary(
            static entry => entry.Key,
            static entry => entry.Value.Serializer,
            StringComparer.Ordinal);

    internal (string ContentType, IZLinkMessageSerializer Serializer)? Fallback =>
        _fallback;

    internal void Add(
        string contentType,
        IZLinkMessageSerializer serializer,
        Func<Type, bool> canSerialize,
        bool isFallback)
    {
        ThrowIfFrozen();
        ArgumentNullException.ThrowIfNull(serializer);
        ArgumentNullException.ThrowIfNull(canSerialize);

        var canonicalContentType = NormalizeRegisteredContentType(contentType);
        _registrations.Remove(canonicalContentType);
        _registrationOrder.Remove(canonicalContentType);
        _registrations[canonicalContentType] = new Registration(
            serializer,
            canSerialize,
            isFallback);
        _registrationOrder.Add(canonicalContentType);

        lock (_cacheGate)
            _resolvedByDeclaredType.Clear();
        RefreshFallback();
    }

    internal bool TryGetExact(
        string contentType,
        out IZLinkMessageSerializer serializer)
    {
        if (!string.IsNullOrEmpty(contentType)
            && _registrations.TryGetValue(contentType, out var registration))
        {
            serializer = registration.Serializer;
            return true;
        }

        serializer = null!;
        return false;
    }

    internal bool TryResolve(
        Type declaredType,
        out string contentType,
        out IZLinkMessageSerializer serializer)
    {
        ArgumentNullException.ThrowIfNull(declaredType);
        if (_resolvedByDeclaredType.TryGetValue(declaredType, out var cached))
            return Return(cached, out contentType, out serializer);

        Resolution resolved;
        lock (_cacheGate)
        {
            if (!_resolvedByDeclaredType.TryGetValue(declaredType, out resolved))
            {
                resolved = ResolveUncached(declaredType);
                if (_resolvedByDeclaredType.Count < MaximumCachedDeclaredTypes)
                    _resolvedByDeclaredType[declaredType] = resolved;
            }
        }

        return Return(resolved, out contentType, out serializer);
    }

    internal ZLinkSerializerSelectionRegistry FrozenCopy()
    {
        var copy = new ZLinkSerializerSelectionRegistry();
        foreach (var contentType in _registrationOrder)
        {
            var registration = _registrations[contentType];
            copy._registrations.Add(contentType, registration);
            copy._registrationOrder.Add(contentType);
        }
        copy.RefreshFallback();
        copy.Freeze();
        return copy;
    }

    internal void Freeze() => Interlocked.Exchange(ref _frozen, 1);

    internal static string NormalizeRegisteredContentType(string contentType)
    {
        ArgumentNullException.ThrowIfNull(contentType);
        var first = 0;
        var last = contentType.Length;
        while (first < last && contentType[first] is ' ' or '\t') first++;
        while (last > first && contentType[last - 1] is ' ' or '\t') last--;

        var slash = -1;
        var normalized = new char[last - first];
        for (var source = first; source < last; source++)
        {
            var value = contentType[source];
            var target = source - first;
            if (value == '/')
            {
                if (slash >= 0 || target == 0 || source == last - 1)
                    throw InvalidContentType();
                slash = target;
                normalized[target] = value;
                continue;
            }

            if (!IsTokenCharacter(value))
                throw InvalidContentType();
            normalized[target] = value is >= 'A' and <= 'Z'
                ? (char)(value + ('a' - 'A'))
                : value;
        }

        if (slash < 0)
            throw InvalidContentType();
        return new string(normalized);
    }

    internal static bool TryNormalizeResponseContentType(
        string? contentType,
        out string normalized)
    {
        if (contentType is null)
        {
            normalized = string.Empty;
            return false;
        }

        var separator = contentType.IndexOf(';');
        try
        {
            normalized = NormalizeRegisteredContentType(
                separator < 0 ? contentType : contentType[..separator]);
            return true;
        }
        catch (ArgumentException)
        {
            normalized = string.Empty;
            return false;
        }
    }

    private Resolution ResolveUncached(Type declaredType)
    {
        for (var index = _registrationOrder.Count - 1; index >= 0; index--)
        {
            var registeredContentType = _registrationOrder[index];
            var registration = _registrations[registeredContentType];
            if (registration.CanSerialize(declaredType))
            {
                return new Resolution(
                    true,
                    registeredContentType,
                    registration.Serializer);
            }
        }

        return new Resolution(false, string.Empty, null);
    }

    private void RefreshFallback()
    {
        _fallback = null;
        foreach (var contentType in _registrationOrder)
        {
            var registration = _registrations[contentType];
            if (registration.IsFallback)
                _fallback = (contentType, registration.Serializer);
        }
    }

    private static bool Return(
        Resolution resolution,
        out string contentType,
        out IZLinkMessageSerializer serializer)
    {
        contentType = resolution.ContentType;
        serializer = resolution.Serializer!;
        return resolution.Found;
    }

    private static bool IsTokenCharacter(char value) =>
        value is >= '0' and <= '9'
        or >= 'A' and <= 'Z'
        or >= 'a' and <= 'z'
        or '!' or '#' or '$' or '%' or '&' or '\'' or '*' or '+' or '-'
        or '.' or '^' or '_' or '`' or '|' or '~';

    private static ArgumentException InvalidContentType() =>
        new(
            "Codec content type must be a parameter-free ASCII type/subtype.",
            "contentType");

    private void ThrowIfFrozen()
    {
        if (Volatile.Read(ref _frozen) != 0)
            throw new InvalidOperationException(
                "The codec registry is immutable after runtime startup.");
    }

    private sealed record Registration(
        IZLinkMessageSerializer Serializer,
        Func<Type, bool> CanSerialize,
        bool IsFallback);

    private readonly record struct Resolution(
        bool Found,
        string ContentType,
        IZLinkMessageSerializer? Serializer);
}
