plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.maxtaube.tidbytwidget"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.maxtaube.tidbytwidget"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"
    }

    // A fixed debug key (android/debug.keystore, not in git) so rebuilt APKs
    // install as updates; each fresh Docker container would otherwise make a new one.
    signingConfigs {
        getByName("debug") {
            storeFile = rootProject.file("debug.keystore")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}
