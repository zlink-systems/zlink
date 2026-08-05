namespace Zlink.Framework.Contracts.Codecs;

/// <summary>
///     Registers a payload codec with the framework codec registry. Built-in codecs
///     and user-defined codecs use the same contract so codec selection changes do
///     not change handler or client payload APIs.
/// </summary>
public interface IZLinkCodecExtension
{
    void Register(IZLinkCodecRegistrar codecs);
}
