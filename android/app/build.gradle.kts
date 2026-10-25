plugins {
    id("com.android.application")
}

android {
    namespace = "com.audiowire"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.audiowire"
        minSdk = 24
        targetSdk = 34
        versionCode = 5
        versionName = "2.3"
    }
    buildTypes {
        release { isMinifyEnabled = false }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
