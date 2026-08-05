using System.Text;
using Zlink.Framework.Contracts.Streams;

namespace Zlink.Framework.Runtime.Backend.DotNet;

// RouteMesh 10.0.0 send/receive application-metadata wire codec (S8-06A).
//
// Core forwards the application metadata frame as an opaque byte blob
// (mesh_wire.cpp copies zlink_mesh_metadata_view_t.data verbatim) and both
// validates it on ingress and rejects over-limit frames via
// mesh_runtime.cpp:validate_metadata. The framework owns the encoding, which
// MUST match that validator byte-for-byte so a snapshot produced by one language
// decodes on every other:
//
//   byte 0        : version, always 1
//   byte 1        : entry count (uint8, 0..255)
//   per entry     : key_len (uint8, 1..255)
//                   key bytes (UTF-8, no NUL)
//                   value_len (uint16, big-endian, 0..65535)
//                   value bytes (UTF-8, no NUL)
//
// Total encoded size (version + count + all entries) MUST be <= 1024 bytes
// (ZLINK_MESH_APPLICATION_METADATA_MAX). Keys are unique (last-write-wins is a
// caller/builder concern; the immutable dictionary guarantees uniqueness here).
internal static class ZLinkMeshMetadataCodec
{
    // ZLINK_MESH_APPLICATION_METADATA_MAX (core/include/zlink/service/mesh_node.h).
    internal const int MaxEncodedSize = 1024;

    private const byte Version = 1;
    private const int MaxEntries = 255;
    private const int MaxKeyLength = 255;
    private const int MaxValueLength = ushort.MaxValue;

    // Snapshots the metadata into the canonical wire frame. Returns an empty
    // buffer when there is nothing to send (no metadata frame is attached), so
    // the binding metadata submitter treats it as "no application metadata".
    // Throws when a caller-provided key/value cannot be represented (over the
    // 1024-byte boundary, too many entries, empty key, non-UTF8, or embedded
    // NUL) so an invalid outbound snapshot fails fast at send time.
    public static ReadOnlyMemory<byte> Encode(ZLinkMessageMetadata? metadata)
    {
        var values = metadata?.Values;
        if (values is null || values.Count == 0)
            return ReadOnlyMemory<byte>.Empty;

        if (values.Count > MaxEntries)
            throw new ArgumentException(
                $"Application metadata may carry at most {MaxEntries} entries, but {values.Count} were provided.",
                nameof(metadata));

        var totalSize = 2;
        foreach (var (key, value) in values)
        {
            ValidateKey(key);
            ValidateValue(key, value);
            totalSize += 1 + Encoding.UTF8.GetByteCount(key)
                + 2 + Encoding.UTF8.GetByteCount(value);
            if (totalSize > MaxEncodedSize)
                throw new ArgumentException(
                    $"Encoded application metadata exceeds the {MaxEncodedSize}-byte limit.",
                    nameof(metadata));
        }

        var buffer = new byte[totalSize];
        buffer[0] = Version;
        buffer[1] = (byte)values.Count;
        var offset = 2;
        foreach (var (key, value) in values)
        {
            var keyLength = Encoding.UTF8.GetBytes(key, 0, key.Length, buffer, offset + 1);
            buffer[offset] = (byte)keyLength;
            offset += 1 + keyLength;

            var valueLength = Encoding.UTF8.GetBytes(value, 0, value.Length, buffer, offset + 2);
            buffer[offset] = (byte)(valueLength >> 8);
            buffer[offset + 1] = (byte)(valueLength & 0xFF);
            offset += 2 + valueLength;
        }

        return buffer;
    }

    internal static int MeasureEncodedLength(ZLinkMessageMetadata? metadata)
    {
        var values = metadata?.Values;
        if (values is null || values.Count == 0)
            return 0;
        if (values.Count > MaxEntries)
            throw new ArgumentException(
                $"Application metadata may carry at most {MaxEntries} entries, but {values.Count} were provided.",
                nameof(metadata));

        var totalSize = 2;
        foreach (var (key, value) in values)
        {
            ValidateKey(key);
            ValidateValue(key, value);
            totalSize = checked(totalSize
                + 1
                + Encoding.UTF8.GetByteCount(key)
                + 2
                + Encoding.UTF8.GetByteCount(value));
            if (totalSize > MaxEncodedSize)
                throw new ArgumentException(
                    $"Encoded application metadata exceeds the {MaxEncodedSize}-byte limit.",
                    nameof(metadata));
        }
        return totalSize;
    }

    // Decodes a received metadata frame into an immutable handler-facing view.
    // Returns true and the decoded snapshot for a well-formed frame (including
    // the empty/absent frame, which yields ZLinkMessageMetadata.Empty). Returns
    // false for any malformed frame so the dispatch pump can reject the ingress
    // as a protocol error and NOT deliver it to a handler (spec 03 §3).
    public static bool TryDecode(
        ReadOnlySpan<byte> frame,
        out ZLinkMessageMetadata metadata)
    {
        metadata = ZLinkMessageMetadata.Empty;
        if (frame.Length == 0)
            return true;

        if (frame.Length > MaxEncodedSize || frame.Length < 2)
            return false;
        if (frame[0] != Version)
            return false;

        int count = frame[1];
        var offset = 2;
        Dictionary<string, string>? values = null;
        for (var entry = 0; entry < count; entry++)
        {
            if (offset + 1 > frame.Length)
                return false;
            int keyLength = frame[offset];
            offset += 1;
            if (keyLength == 0 || offset + keyLength > frame.Length)
                return false;
            var keySpan = frame.Slice(offset, keyLength);
            if (!TryDecodeUtf8(keySpan, out var key))
                return false;
            offset += keyLength;

            if (offset + 2 > frame.Length)
                return false;
            int valueLength = (frame[offset] << 8) | frame[offset + 1];
            offset += 2;
            if (offset + valueLength > frame.Length)
                return false;
            var valueSpan = frame.Slice(offset, valueLength);
            if (!TryDecodeUtf8(valueSpan, out var value))
                return false;
            offset += valueLength;

            values ??= new Dictionary<string, string>(count, StringComparer.Ordinal);
            if (!values.TryAdd(key, value))
                return false;
        }

        if (offset != frame.Length)
            return false;

        metadata = values is null
            ? ZLinkMessageMetadata.Empty
            : new ZLinkMessageMetadata(values);
        return true;
    }

    private static void ValidateKey(string key)
    {
        if (string.IsNullOrEmpty(key))
            throw new ArgumentException("Application metadata keys must not be empty.");
        var length = Encoding.UTF8.GetByteCount(key);
        if (length is 0 or > MaxKeyLength)
            throw new ArgumentException(
                $"Application metadata key '{key}' must encode to 1..{MaxKeyLength} UTF-8 bytes.");
        if (key.Contains('\0'))
            throw new ArgumentException(
                $"Application metadata key '{key}' must not contain a NUL character.");
    }

    private static void ValidateValue(string key, string value)
    {
        ArgumentNullException.ThrowIfNull(value);
        if (Encoding.UTF8.GetByteCount(value) > MaxValueLength)
            throw new ArgumentException(
                $"Application metadata value for key '{key}' exceeds {MaxValueLength} UTF-8 bytes.");
        if (value.Contains('\0'))
            throw new ArgumentException(
                $"Application metadata value for key '{key}' must not contain a NUL character.");
    }

    private static bool TryDecodeUtf8(ReadOnlySpan<byte> bytes, out string text)
    {
        if (bytes.IndexOf((byte)0) >= 0)
        {
            text = string.Empty;
            return false;
        }

        try
        {
            text = new UTF8Encoding(false, true).GetString(bytes);
            return true;
        }
        catch (DecoderFallbackException)
        {
            text = string.Empty;
            return false;
        }
    }
}
