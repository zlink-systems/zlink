plugins {
    base
    idea
    id("org.jetbrains.kotlin.jvm") version "2.1.0" apply false
    id("org.jetbrains.kotlin.plugin.spring") version "2.1.0" apply false
}

idea {
    module {
        name = "zlink-framework-java-samples"
    }
}

val sampleProjectPaths = listOf(
    ":java:Bingo:Client",
    ":java:Bingo:Server:Api",
    ":java:Bingo:Server:Configuration",
    ":java:Bingo:Server:Matchmaking",
    ":java:Bingo:Server:Play",
    ":java:Bingo:Server:Session",
    ":java:Bingo:Shared",
    ":java:DeliveryDispatch:Client",
    ":java:DeliveryDispatch:Server:Configuration",
    ":java:DeliveryDispatch:Server:CourierSession",
    ":java:DeliveryDispatch:Server:CourierSpotNode",
    ":java:DeliveryDispatch:Server:CustomerGateway",
    ":java:DeliveryDispatch:Server:Dispatch",
    ":java:DeliveryDispatch:Server:Tracking",
    ":java:DeliveryDispatch:Shared",
    ":java:GameQuest:Client",
    ":java:GameQuest:Server:Configuration",
    ":java:GameQuest:Server:GameApi",
    ":java:GameQuest:Server:QuestMission",
    ":java:GameQuest:Shared",
    ":java:ShoppingMall:Client",
    ":java:ShoppingMall:Server:CommerceApi",
    ":java:ShoppingMall:Server:Configuration",
    ":java:ShoppingMall:Server:OrderWorkflow",
    ":java:ShoppingMall:Server:Shared",
    ":java:ShoppingMall:Shared",
    ":java:TicTacToe:Client",
    ":java:TicTacToe:Server",
    ":java:TicTacToe:Shared",
    ":java:SupportChat:Client",
    ":java:SupportChat:Server:Api",
    ":java:SupportChat:Server:Configuration",
    ":java:SupportChat:Server:Session",
    ":java:SupportChat:Server:Support",
    ":java:SupportChat:Shared",
    ":kotlin:Bingo:Client",
    ":kotlin:Bingo:Server:Api",
    ":kotlin:Bingo:Server:Configuration",
    ":kotlin:Bingo:Server:Matchmaking",
    ":kotlin:Bingo:Server:Play",
    ":kotlin:Bingo:Server:Session",
    ":kotlin:Bingo:Shared",
    ":kotlin:DeliveryDispatch:Client",
    ":kotlin:DeliveryDispatch:Server:Configuration",
    ":kotlin:DeliveryDispatch:Server:CourierSession",
    ":kotlin:DeliveryDispatch:Server:CourierSpotNode",
    ":kotlin:DeliveryDispatch:Server:CustomerGateway",
    ":kotlin:DeliveryDispatch:Server:Dispatch",
    ":kotlin:DeliveryDispatch:Server:Tracking",
    ":kotlin:DeliveryDispatch:Shared",
    ":kotlin:GameQuest:Client",
    ":kotlin:GameQuest:Server:Configuration",
    ":kotlin:GameQuest:Server:GameApi",
    ":kotlin:GameQuest:Server:QuestMission",
    ":kotlin:GameQuest:Shared",
    ":kotlin:ShoppingMall:Client",
    ":kotlin:ShoppingMall:Server:CommerceApi",
    ":kotlin:ShoppingMall:Server:Configuration",
    ":kotlin:ShoppingMall:Server:OrderWorkflow",
    ":kotlin:ShoppingMall:Shared",
    ":kotlin:TicTacToe:Client",
    ":kotlin:TicTacToe:Server",
    ":kotlin:TicTacToe:Shared",
    ":kotlin:SupportChat:Client",
    ":kotlin:SupportChat:Server:Api",
    ":kotlin:SupportChat:Server:Configuration",
    ":kotlin:SupportChat:Server:Session",
    ":kotlin:SupportChat:Server:Support",
    ":kotlin:SupportChat:Shared",
)

tasks.register("buildAllSamples") {
    group = LifecycleBasePlugin.BUILD_GROUP
    description = "Builds every Java and Kotlin ZLink sample included in this IDE project."
    dependsOn(sampleProjectPaths.map { "$it:build" })
}

tasks.register("cleanAllSamples") {
    group = LifecycleBasePlugin.BUILD_GROUP
    description = "Cleans every Java and Kotlin ZLink sample included in this IDE project."
    dependsOn(sampleProjectPaths.map { "$it:clean" })
}

tasks.register("verifyPackageMode") {
    group = LifecycleBasePlugin.VERIFICATION_GROUP
    description = "Verifies that samples resolve published packages without composite source builds."
    doLast {
        check(providers.gradleProperty("zlink.samples.packageMode").orNull == "true") {
            "Run with -Pzlink.samples.packageMode=true."
        }
        check(gradle.includedBuilds.isEmpty()) {
            "Package mode must not include framework or bindings source builds: " +
                gradle.includedBuilds.joinToString { it.name }
        }
        println("ZLINK_SAMPLE_INCLUDED_BUILD_COUNT=0")
    }
}
