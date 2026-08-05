package systems.zlink.e2e.kotlin.spotservice.session

fun main(args: Array<String>) {
    SessionApplication.run(*args).use {
        Thread.currentThread().join()
    }
}
