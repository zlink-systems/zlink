package systems.zlink.e2e.kotlin.registrationcodec

import systems.zlink.e2e.kotlin.registrationcodec.codecrequester.runCodecRequesterApplication

fun main(args: Array<String>) {
    Env.configure(args)
    runCodecRequesterApplication(*args)
}
