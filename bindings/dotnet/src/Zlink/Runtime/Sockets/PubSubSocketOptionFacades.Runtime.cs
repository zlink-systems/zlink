// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

public sealed partial class StreamSocketOptions
{
    internal StreamSocketOptions(ISocketOptionEndpoint socket)
        : base(socket)
    {
    }
}

public sealed partial class PubSocketOptions
{
    internal PubSocketOptions(ISocketOptionEndpoint socket)
        : base(socket)
    {
    }
}

public sealed partial class SubSocketOptions
{
    internal SubSocketOptions(ISocketOptionEndpoint socket)
        : base(socket)
    {
    }
}