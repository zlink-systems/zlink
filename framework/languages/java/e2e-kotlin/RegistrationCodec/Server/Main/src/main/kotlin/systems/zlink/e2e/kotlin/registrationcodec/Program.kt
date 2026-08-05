package systems.zlink.e2e.kotlin.registrationcodec

import systems.zlink.e2e.kotlin.registrationcodec.main.runServerApplication

fun main(args: Array<String>) {
    Env.configure(args)
    runServerApplication(*args)
}
