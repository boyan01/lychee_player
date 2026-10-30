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
    buildTypes {
        release {
            isMinifyEnabled = true
            signingConfig = signingConfigs.getByName("debug")
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
        }
    }
}

dependencies {
    val published = providers.gradleProperty("lychee.example.repo").isPresent
    val sdkVersion = providers.gradleProperty("lychee.example.version").orNull
        ?: Regex("""(?m)^version: (\S+)""")
            .find(rootDir.resolve("../../flutter/pubspec.yaml").readText())!!.groupValues[1]
    if (published) {
        implementation("io.github.boyan01.lychee:lychee:$sdkVersion")
    } else {
        implementation(project(":lychee"))
    }
    if (providers.gradleProperty("lychee.example.ffmpeg").orNull == "true") {
        if (published) {
            implementation("io.github.boyan01.lychee:lychee-ffmpeg:$sdkVersion")
        } else {
            implementation(project(":ffmpeg"))
        }
    }
}
