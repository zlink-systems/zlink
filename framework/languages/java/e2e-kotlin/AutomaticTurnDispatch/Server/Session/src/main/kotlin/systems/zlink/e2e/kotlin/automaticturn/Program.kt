package systems.zlink.e2e.kotlin.automaticturn

fun main(args: Array<String>) {
    Env.configure(args)
    SessionApplication.run().use {
        Thread.currentThread().join()
    }
}
