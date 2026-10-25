plugins {
    id("com.android.application")
}

android {
    namespace = "tech.soit.lychee.example"
    compileSdk = 36

    defaultConfig {
        applicationId = "tech.soit.lychee.example"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation(project(":lychee"))
}
