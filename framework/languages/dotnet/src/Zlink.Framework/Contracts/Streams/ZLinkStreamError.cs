namespace Zlink.Framework.Contracts.Streams;

public readonly record struct ZLinkStreamError(
    ZLinkStreamSessionError Error,
    string? Message);
