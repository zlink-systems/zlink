/* SPDX-License-Identifier: Apache-2.0 */

using System.IO.Compression;

namespace Zlink.HttpClient.Runtime;

/// <summary>
///     Wrapper-controlled response decompression mirroring the C++ <c>compression.cpp</c>:
///     gzip and deflate are decoded, the decoded size is bounded by the configured body limit, and
///     the caller removes the <c>Content-Encoding</c> header afterwards. Native auto-decompression is
///     disabled so streaming downloads are never transparently decoded and the body limit is enforced
///     against the decoded size. A malformed body raises <see cref="ZLinkFrameworkErrorKind.ProtocolError" />;
///     exceeding the configured limit raises <see cref="ZLinkFrameworkErrorKind.Rejected" />
///     with <see cref="ZLinkRetryAdvice.DoNotRetry" />.
/// </summary>
internal static class ResponseCompression
{
    internal const string GzipEncoding = "gzip";
    internal const string DeflateEncoding = "deflate";
    internal const string AcceptedEncodings = GzipEncoding + ", " + DeflateEncoding;
    private const int ZlibHeaderSize = sizeof(ushort);
    private const byte CompressionMethodMask = 0x0f;
    private const byte DeflateCompressionMethod = 8;
    private const int ZlibHeaderCheckDivisor = 31;
    private const int BitsPerByte = 8;

    public static byte[] Gunzip(byte[] input, long maxBytes)
    {
        return Decode(
            () => new GZipStream(new MemoryStream(input), CompressionMode.Decompress),
            maxBytes
        );
    }

    public static byte[] InflateDeflate(byte[] input, long maxBytes)
    {
        return Decode(
            () =>
                IsZlibWrapped(input)
                    ? new ZLibStream(new MemoryStream(input), CompressionMode.Decompress)
                    : new DeflateStream(new MemoryStream(input), CompressionMode.Decompress),
            maxBytes
        );
    }

    // Matches the C++ heuristic: a zlib stream begins with CMF/FLG bytes where the compression
    // method is deflate (low nibble 8) and the 16-bit header is a multiple of 31.
    private static bool IsZlibWrapped(byte[] input)
    {
        return input.Length >= ZlibHeaderSize
            && (input[0] & CompressionMethodMask) == DeflateCompressionMethod
            && (((input[0] << BitsPerByte) | input[1]) % ZlibHeaderCheckDivisor) == 0;
    }

    private static byte[] Decode(Func<Stream> open, long maxBytes)
    {
        try
        {
            using var decompressor = open();
            using var output = new MemoryStream();
            var buffer = new byte[ResponseBodyReader.ReadBufferSize];
            int read;
            while ((read = decompressor.Read(buffer, 0, buffer.Length)) > 0)
            {
                if (output.Length + read > maxBytes)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Rejected,
                        "HTTP response compressed body exceeds max_response_body_size",
                        ZLinkRetryAdvice.DoNotRetry
                    );

                output.Write(buffer, 0, read);
            }

            return output.ToArray();
        }
        catch (InvalidDataException ex)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "HTTP response compressed body is malformed",
                innerException: ex
            );
        }
    }
}
