plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

kotlin {
    jvmToolchain(25)
}

dependencies {
    implementation(project(":kotlin:Shared"))
    // This process is outside the mesh. It references the connector only, never
    // the Framework.
    implementation(libs.zlink.stream.connector)
    // The connector's JSON codec (de)serializes the Kotlin data classes in Shared.
    implementation(libs.jackson.module.kotlin)
}

application {
    mainClass.set("systems.zlink.tutorial.streamclient.StreamClientProgramKt")
}
