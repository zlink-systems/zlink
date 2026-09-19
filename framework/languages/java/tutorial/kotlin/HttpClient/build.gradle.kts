plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

kotlin {
    jvmToolchain(25)
}

dependencies {
    // This process is outside the mesh and references only the HTTP client wrapper.
    implementation(libs.zlink.http.client.kotlin)
}

application {
    mainClass.set("systems.zlink.tutorial.httpclient.HttpClientProgramKt")
}
