plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}


application {
    applicationName = "observability-ops-kotlin-a5-client"
    mainClass.set("systems.zlink.e2e.kotlin.observabilityops.a5.client.ProgramKt")
}
