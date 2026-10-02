package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.sockets.RecvResult;

import java.util.concurrent.CompletionException;

final class ZLinkChannelReceiveResultTest {
    @ParameterizedTest
    @EnumSource(value = RecvResult.class, mode = EnumSource.Mode.EXCLUDE, names = "OK")
    void wrappedReceiveErrorsRetainTheirTypedResult(RecvResult result) {
        var failure = new CompletionException(new ZlinkRecvException(result));
        assertEquals(result == RecvResult.NO_DATA, ZLinkChannelRuntime.isNoDataReceive(failure));
    }
}
