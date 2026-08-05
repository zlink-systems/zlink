using System.Text;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Timers;

namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotTimerRelocationCodec
{
    private const uint Magic = 0x5a4c5452; // ZLTR
    private const ushort Version = 1;
    private const int MaxPayloadBytes = 1024 * 1024;

    internal static ZLinkRelocationLogicalTimer Encode(
        ZLinkSpotLogicalTimerSnapshot snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
        writer.Write(Magic);
        writer.Write(Version);
        WriteString(writer, snapshot.HandlerType.AssemblyQualifiedName
                            ?? snapshot.HandlerType.FullName
                            ?? snapshot.HandlerType.Name);
        WriteString(writer, snapshot.SpotType.AssemblyQualifiedName
                            ?? snapshot.SpotType.FullName
                            ?? snapshot.SpotType.Name);
        var timer = snapshot.Timer;
        writer.Write((byte)timer.Options.OverrunPolicy);
        writer.Write(timer.Options.MaxCatchUpTicks);
        writer.Write(timer.Options.StopOnUnhandledException);
        writer.Write(timer.StartedAt.UtcTicks);
        writer.Write(timer.DeliveryIndex);
        writer.Write(timer.LastScheduledIndex);
        writer.Write(timer.NextScheduledAt.HasValue);
        if (timer.NextScheduledAt is { } next)
            writer.Write(next.UtcTicks);
        writer.Write(timer.PendingTick.HasValue);
        if (timer.PendingTick is { } pending)
            WriteTick(writer, pending);
        writer.Flush();
        if (stream.Length > MaxPayloadBytes)
            throw new InvalidOperationException(
                "A logical timer relocation payload cannot exceed 1 MiB.");

        var due = timer.PendingTick?.ScheduledAt
                  ?? timer.NextScheduledAt
                  ?? timer.StartedAt + timer.Period;
        return new ZLinkRelocationLogicalTimer(
            timer.Name,
            due.ToUnixTimeMilliseconds(),
            Math.Max(1, checked((long)Math.Ceiling(timer.Period.TotalMilliseconds))),
            stream.ToArray());
    }

    internal static ZLinkSpotLogicalTimerSnapshot Decode(
        ZLinkRelocationLogicalTimer relocation,
        Type? canonicalSpotType = null)
    {
        if (relocation.CanonicalTimer is { } canonical)
            return DecodeCanonical(relocation, canonical, canonicalSpotType);
        if (relocation.Payload.Length is <= 0 or > MaxPayloadBytes)
            throw new InvalidDataException(
                "The logical timer relocation payload size is invalid.");
        using var stream = new MemoryStream(
            relocation.Payload.ToArray(),
            writable: false);
        using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: true);
        if (reader.ReadUInt32() != Magic || reader.ReadUInt16() != Version)
            throw new InvalidDataException(
                "The logical timer relocation payload header is invalid.");
        var handlerType = ResolveType(ReadString(reader));
        var spotType = ResolveType(ReadString(reader));
        var policy = (ZLinkTimerOverrunPolicy)reader.ReadByte();
        if (!Enum.IsDefined(policy))
            throw new InvalidDataException(
                "The logical timer overrun policy is invalid.");
        var maxCatchUpTicks = reader.ReadInt32();
        var stopOnUnhandledException = reader.ReadBoolean();
        if (policy == ZLinkTimerOverrunPolicy.CatchUpBounded
            && maxCatchUpTicks <= 0)
            throw new InvalidDataException(
                "The logical timer catch-up bound is invalid.");
        var startedAt = new DateTimeOffset(reader.ReadInt64(), TimeSpan.Zero);
        var deliveryIndex = reader.ReadUInt64();
        var lastScheduledIndex = reader.ReadUInt64();
        var next = reader.ReadBoolean()
            ? new DateTimeOffset(reader.ReadInt64(), TimeSpan.Zero)
            : (DateTimeOffset?)null;
        var pending = reader.ReadBoolean()
            ? ReadTick(reader, relocation.TimerId)
            : (ZLinkTimerTick?)null;
        if (stream.Position != stream.Length)
            throw new InvalidDataException(
                "The logical timer relocation payload contains trailing bytes.");
        var period = TimeSpan.FromMilliseconds(relocation.PeriodMilliseconds);
        if (period <= TimeSpan.Zero)
            throw new InvalidDataException(
                "The logical timer period is invalid.");
        return new ZLinkSpotLogicalTimerSnapshot(
            handlerType,
            spotType,
            new ZLinkTimerLogicalSnapshot(
                relocation.TimerId,
                period,
                new ZLinkTimerOptions
                {
                    OverrunPolicy = policy,
                    MaxCatchUpTicks = maxCatchUpTicks,
                    StopOnUnhandledException = stopOnUnhandledException
                },
                startedAt,
                deliveryIndex,
                lastScheduledIndex,
                next,
                pending));
    }

    private static ZLinkSpotLogicalTimerSnapshot DecodeCanonical(
        ZLinkRelocationLogicalTimer relocation,
        ZLinkCanonicalLogicalTimer canonical,
        Type? spotType)
    {
        if (spotType is null)
            throw new InvalidDataException(
                "The canonical logical timer requires its restored SPOT type.");
        var handlerType = ResolveType(canonical.HandlerType);
        var policy = (ZLinkTimerOverrunPolicy)canonical.OverrunPolicy;
        if (!Enum.IsDefined(policy)
            || canonical.MaxCatchUpTicks is 0 or > int.MaxValue)
            throw new InvalidDataException(
                "The canonical logical timer options are invalid.");
        var period = TimeSpan.FromMilliseconds(relocation.PeriodMilliseconds);
        var next = DateTimeOffset.FromUnixTimeMilliseconds(
            canonical.NextScheduledAtUnixMilliseconds);
        var startedAt = next - period;
        ZLinkTimerTick? pending = null;
        if (canonical.PendingTick is { } tick)
        {
            var scheduledAt = DateTimeOffset.FromUnixTimeMilliseconds(
                tick.ScheduledAtUnixMilliseconds);
            pending = new ZLinkTimerTick(
                relocation.TimerId,
                tick.DeliveryIndex,
                tick.ScheduledIndex,
                period,
                scheduledAt,
                scheduledAt,
                TimeSpan.Zero,
                TimeSpan.Zero,
                TimeSpan.Zero,
                tick.SkippedTicks);
        }
        return new ZLinkSpotLogicalTimerSnapshot(
            handlerType,
            spotType,
            new ZLinkTimerLogicalSnapshot(
                relocation.TimerId,
                period,
                new ZLinkTimerOptions
                {
                    OverrunPolicy = policy,
                    MaxCatchUpTicks = checked((int)canonical.MaxCatchUpTicks),
                    StopOnUnhandledException = canonical.StopOnUnhandledException
                },
                startedAt,
                canonical.LastCompletedDeliveryIndex,
                canonical.LastCompletedScheduledIndex,
                next,
                pending));
    }

    private static void WriteTick(BinaryWriter writer, ZLinkTimerTick tick)
    {
        writer.Write(tick.DeliveryIndex);
        writer.Write(tick.ScheduledIndex);
        writer.Write(tick.Period.Ticks);
        writer.Write(tick.ScheduledAt.UtcTicks);
        writer.Write(tick.StartedAt.UtcTicks);
        writer.Write(tick.ScheduledElapsed.Ticks);
        writer.Write(tick.StartedElapsed.Ticks);
        writer.Write(tick.Delay.Ticks);
        writer.Write(tick.SkippedTicks);
    }

    private static ZLinkTimerTick ReadTick(BinaryReader reader, string name)
    {
        return new ZLinkTimerTick(
            name,
            reader.ReadUInt64(),
            reader.ReadUInt64(),
            TimeSpan.FromTicks(reader.ReadInt64()),
            new DateTimeOffset(reader.ReadInt64(), TimeSpan.Zero),
            new DateTimeOffset(reader.ReadInt64(), TimeSpan.Zero),
            TimeSpan.FromTicks(reader.ReadInt64()),
            TimeSpan.FromTicks(reader.ReadInt64()),
            TimeSpan.FromTicks(reader.ReadInt64()),
            reader.ReadUInt64());
    }

    private static Type ResolveType(string assemblyQualifiedName)
    {
        return Type.GetType(
                   assemblyQualifiedName,
                   throwOnError: false,
                   ignoreCase: false)
               ?? throw new InvalidDataException(
                   $"Logical timer handler type '{assemblyQualifiedName}' is unavailable.");
    }

    private static void WriteString(BinaryWriter writer, string value)
    {
        var bytes = Encoding.UTF8.GetBytes(value);
        if (bytes.Length is <= 0 or > ushort.MaxValue)
            throw new InvalidOperationException(
                "A logical timer type name is outside its bound.");
        writer.Write((ushort)bytes.Length);
        writer.Write(bytes);
    }

    private static string ReadString(BinaryReader reader)
    {
        var length = reader.ReadUInt16();
        if (length == 0)
            throw new InvalidDataException(
                "A logical timer type name is empty.");
        var bytes = reader.ReadBytes(length);
        if (bytes.Length != length)
            throw new EndOfStreamException();
        return Encoding.UTF8.GetString(bytes);
    }
}
