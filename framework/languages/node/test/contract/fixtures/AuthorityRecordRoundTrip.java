import java.lang.reflect.Method;

// Uses the Java repository codec so this test exercises its stored bytes.
class AuthorityRecordRoundTrip {
    public static void main(String[] args) throws Exception {
        Class<?> repository = Class.forName(
                "systems.zlink.framework.runtime.internal.locations.ZLinkProviderAuthorityRepository");
        Method decode = repository.getDeclaredMethod("decode", byte[].class);
        decode.setAccessible(true);
        Object record = decode.invoke(null, (Object) System.in.readAllBytes());
        Method encode = repository.getDeclaredMethod("encode", record.getClass());
        encode.setAccessible(true);
        System.out.write((byte[]) encode.invoke(null, record));
    }
}
