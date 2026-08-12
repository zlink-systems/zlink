plugins {
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation("com.fasterxml.jackson.module:jackson-module-kotlin:2.17.2")
    implementation(kotlin("stdlib"))
}

kotlin {
    jvmToolchain(22)
}
