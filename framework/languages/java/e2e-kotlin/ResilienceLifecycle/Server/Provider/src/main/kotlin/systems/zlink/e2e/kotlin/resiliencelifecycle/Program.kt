package systems.zlink.e2e.kotlin.resiliencelifecycle

fun main(args: Array<String>) {
    ProviderApplication.run(*args).use { _ ->
        Thread.currentThread().join()
    }
}
