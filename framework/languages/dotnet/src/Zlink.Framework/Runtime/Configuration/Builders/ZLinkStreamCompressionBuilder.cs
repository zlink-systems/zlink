namespace Zlink.Framework.Runtime.Configuration.Builders;

internal sealed class ZLinkStreamCompressionBuilder(
    ZLinkFrameworkRegistration registration) : IZLinkStreamCompressionBuilder
{
    public IZLinkStreamCompressionBuilder UseDefault()
    {
        registration.StreamCompressionCodec = ZLinkStreamProtocolDefaults.CreateLz4CompressionCodec();
        return this;
    }

    public IZLinkStreamCompressionBuilder UseLz4()
    {
        return UseDefault();
    }

    public IZLinkStreamCompressionBuilder Use(IZlinkStreamCompressionCodec codec)
    {
        registration.StreamCompressionCodec = codec ?? throw new ArgumentNullException(nameof(codec));
        return this;
    }

    public IZLinkStreamCompressionBuilder Disable()
    {
        registration.StreamCompressionCodec = null;
        return this;
    }
}