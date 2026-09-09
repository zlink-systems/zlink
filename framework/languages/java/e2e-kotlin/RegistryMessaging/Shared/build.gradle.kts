plugins {
    id("org.jetbrains.kotlin.jvm")
}


dependencies {
    implementation("systems.zlink:zlink-framework-core:0.10.0")
    api("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    api("com.fasterxml.jackson.module:jackson-module-kotlin:2.17.2")
}
