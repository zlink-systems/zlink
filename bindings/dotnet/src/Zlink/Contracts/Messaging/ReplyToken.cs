// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;

namespace Systems.Zlink;

/// <summary>
///     An opaque capability for replying to one ROUTER request.
/// </summary>
public sealed class ReplyToken : IEquatable<ReplyToken>
{
    private readonly object _owner;
    private readonly ulong _value;

    internal ReplyToken(object owner, ulong value)
    {
        _owner = owner ?? throw new ArgumentNullException(nameof(owner));
        if (value == 0)
            throw new ArgumentOutOfRangeException(nameof(value));
        _value = value;
    }

    /// <inheritdoc />
    public bool Equals(ReplyToken? other)
    {
        return other is not null
               && ReferenceEquals(_owner, other._owner)
               && _value == other._value;
    }

    /// <inheritdoc />
    public override bool Equals(object? obj)
    {
        return obj is ReplyToken other && Equals(other);
    }

    /// <inheritdoc />
    public override int GetHashCode()
    {
        return HashCode.Combine(RuntimeHelpers.GetHashCode(_owner), _value);
    }

    /// <inheritdoc />
    public override string ToString()
    {
        return nameof(ReplyToken);
    }

    internal bool IsOwnedBy(object owner)
    {
        return ReferenceEquals(_owner, owner);
    }

    internal ulong Value => _value;
}
