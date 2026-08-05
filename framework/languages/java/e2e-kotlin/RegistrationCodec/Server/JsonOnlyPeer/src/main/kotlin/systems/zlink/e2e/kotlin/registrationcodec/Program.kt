package systems.zlink.e2e.kotlin.registrationcodec

import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.runServerApplication

fun main(args: Array<String>) {
    Env.configure(args)
    runServerApplication(*args)
}
