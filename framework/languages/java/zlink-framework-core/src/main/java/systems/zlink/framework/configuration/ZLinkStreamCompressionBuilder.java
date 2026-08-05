package systems.zlink.framework.configuration;

import systems.zlink.framework.streams.ZLinkStreamCompressionCodec;

public interface ZLinkStreamCompressionBuilder {
    ZLinkStreamCompressionBuilder useDefault();

    ZLinkStreamCompressionBuilder useLz4();

    ZLinkStreamCompressionBuilder use(ZLinkStreamCompressionCodec codec);

    ZLinkStreamCompressionBuilder disable();
}
