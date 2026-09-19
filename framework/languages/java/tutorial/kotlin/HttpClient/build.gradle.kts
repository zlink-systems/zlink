plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

kotlin {
    jvmToolchain(25)
}

dependencies {
    // --8<-- [start:http-client-dependency]
    // This process is outside the mesh and references only the HTTP client wrapper.
    implementation(libs.zlink.http.client.kotlin)
    // --8<-- [end:http-client-dependency]
}

application {
    mainClass.set("systems.zlink.tutorial.httpclient.HttpClientProgramKt")
}
