package systems.zlink.e2e.kotlin.registrationcodec

fun main(args: Array<String>) {
    Env.configure(args)
    runInvalidServerApplication(*args)
}
