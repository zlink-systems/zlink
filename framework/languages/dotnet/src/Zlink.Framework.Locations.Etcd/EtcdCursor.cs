using System.Diagnostics;
using System.Security.Cryptography;
using Google.Protobuf;

namespace Zlink.Framework.Locations.Etcd;

internal sealed record EtcdSnapshot(
    ulong Cluster,
    long Revision,
    long Now,
    long Started,
    ByteString Last
);

internal sealed class EtcdCursor
{
    private readonly byte[] secret = RandomNumberGenerator.GetBytes(32);

    internal string Encode(EtcdSnapshot snapshot, string ns, ByteString prefix)
    {
        using var buffer = new MemoryStream();
        using (var writer = new BinaryWriter(buffer, EtcdEncoding.Utf8, true))
        {
            writer.Write(snapshot.Cluster);
            writer.Write(snapshot.Revision);
            writer.Write(snapshot.Now);
            writer.Write(snapshot.Started);
            Write(writer, ByteString.CopyFromUtf8(ns));
            Write(writer, prefix);
            Write(writer, snapshot.Last);
        }
        var data = buffer.ToArray();
        var bytes = data.Concat(HMACSHA256.HashData(secret, data)).ToArray();
        var result = Convert.ToBase64String(bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_');
        if (result.Length > 4096)
            throw EtcdEncoding.Failure();
        return result;
    }

    private static void Write(BinaryWriter writer, ByteString bytes)
    {
        writer.Write((ushort)bytes.Length);
        writer.Write(bytes.Span);
    }

    private static ByteString Read(BinaryReader reader)
    {
        var length = reader.ReadUInt16();
        var bytes = reader.ReadBytes(length);
        if (bytes.Length != length)
            throw new EndOfStreamException();
        return ByteString.CopyFrom(bytes);
    }

    internal EtcdSnapshot? Decode(string value, string ns, ByteString prefix)
    {
        if (
            string.IsNullOrEmpty(value)
            || value.Length > 4096
            || value.Any(c => !(char.IsAsciiLetterOrDigit(c) || c is '-' or '_'))
        )
            return null;
        try
        {
            var encoded = value.Replace('-', '+').Replace('_', '/');
            var bytes = Convert.FromBase64String(
                encoded.PadRight((encoded.Length + 3) / 4 * 4, '=')
            );
            if (bytes.Length < 70)
                return null;
            var length = bytes.Length - 32;
            if (
                !CryptographicOperations.FixedTimeEquals(
                    bytes.AsSpan(length),
                    HMACSHA256.HashData(secret, bytes.AsSpan(0, length))
                )
            )
                return null;
            using var stream = new MemoryStream(bytes, 0, length, false);
            using var reader = new BinaryReader(stream, EtcdEncoding.Utf8);
            var cluster = reader.ReadUInt64();
            var revision = reader.ReadInt64();
            var now = reader.ReadInt64();
            var started = reader.ReadInt64();
            if (Read(reader) != ByteString.CopyFromUtf8(ns) || Read(reader) != prefix)
                return null;
            var last = Read(reader);
            if (
                stream.Position != length
                || revision <= 0
                || now is < 0 or > EtcdEncoding.MaxTime
                || last.Length is < 1 or > 1024
                || started > Stopwatch.GetTimestamp()
                || Stopwatch.GetElapsedTime(started) >= TimeSpan.FromMinutes(5)
            )
                return null;
            return new(cluster, revision, now, started, last);
        }
        catch (FormatException)
        {
            return null;
        }
        catch (IOException)
        {
            return null;
        }
    }
}
