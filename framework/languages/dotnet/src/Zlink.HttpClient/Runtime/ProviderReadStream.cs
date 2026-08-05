/* SPDX-License-Identifier: Apache-2.0 */

namespace Zlink.HttpClient.Runtime;

/// <summary>
///     Read-only <see cref="Stream" /> that pulls request body chunks from a provider. Wrapped in a
///     <see cref="StreamContent" /> with chunked transfer-encoding so the runtime streams the body
///     without buffering. The provider returns <c>null</c> when the body is complete. Mirrors the C++
///     <c>body_stream</c> path; such requests are excluded from automatic retry because the provider
///     cannot be rewound.
/// </summary>
internal sealed class ProviderReadStream(Func<byte[]?> provider) : Stream
{
    private bool _completed;
    private byte[] _current = Array.Empty<byte>();
    private int _position;

    public override bool CanRead => true;

    public override bool CanSeek => false;

    public override bool CanWrite => false;

    public override long Length => throw new NotSupportedException();

    public override long Position
    {
        get => throw new NotSupportedException();
        set => throw new NotSupportedException();
    }

    public override int Read(byte[] buffer, int offset, int count)
    {
        return Read(buffer.AsSpan(offset, count));
    }

    public override int Read(Span<byte> buffer)
    {
        if (!EnsureCurrent()) return 0;

        var take = Math.Min(buffer.Length, _current.Length - _position);
        _current.AsSpan(_position, take).CopyTo(buffer);
        _position += take;
        return take;
    }

    public override ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(Read(buffer.Span));
    }

    public override Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken cancellationToken)
    {
        return ReadAsync(buffer.AsMemory(offset, count), cancellationToken).AsTask();
    }

    // Pulls chunks until a non-empty one is available or the provider signals completion.
    private bool EnsureCurrent()
    {
        while (_position >= _current.Length)
        {
            if (_completed) return false;

            var next = provider();
            if (next is null)
            {
                _completed = true;
                return false;
            }

            _current = next;
            _position = 0;
        }

        return true;
    }

    public override void Flush()
    {
    }

    public override long Seek(long offset, SeekOrigin origin)
    {
        throw new NotSupportedException();
    }

    public override void SetLength(long value)
    {
        throw new NotSupportedException();
    }

    public override void Write(byte[] buffer, int offset, int count)
    {
        throw new NotSupportedException();
    }
}
