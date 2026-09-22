plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

kotlin {
    jvmToolchain(25)
}

dependencies {
    implementation(project(":kotlin:Shared"))
    // The connector is the transport surface; the Kotlin module supplies its
    // coroutine wrapper.
    implementation(libs.zlink.stream.connector)
    implementation(libs.zlink.framework.kotlin)
    // The connector's JSON codec (de)serializes the Kotlin data classes in Shared.
    implementation(libs.jackson.module.kotlin)
}

application {
    mainClass.set("systems.zlink.tutorial.streamclient.StreamClientProgramKt")
}
