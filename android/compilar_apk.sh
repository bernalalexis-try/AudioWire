#!/bin/sh
set -e
cd "$(dirname "$0")"
B=build_apk; rm -rf $B; mkdir -p $B/obj
[ -f android-34.jar ] || curl -sSL -o android-34.jar \
  https://raw.githubusercontent.com/Sable/android-platforms/master/android-34/android.jar

sed 's|<manifest xmlns:android="http://schemas.android.com/apk/res/android">|<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.audiowire" android:versionCode="4" android:versionName="2.2">\n    <uses-sdk android:minSdkVersion="24" android:targetSdkVersion="34" />|' \
  app/src/main/AndroidManifest.xml > $B/AndroidManifest.xml

javac -encoding UTF-8 -nowarn -Xlint:-options -source 8 -target 8 -bootclasspath android-34.jar -d $B/obj \
  app/src/main/java/com/audiowire/*.java
dalvik-exchange --dex --min-sdk-version=24 --output=$B/classes.dex $B/obj
aapt package -f -M $B/AndroidManifest.xml -S app/src/main/res -I android-34.jar -F $B/sin_firmar.apk
(cd $B && aapt add sin_firmar.apk classes.dex >/dev/null)
zipalign -f 4 $B/sin_firmar.apk $B/alineado.apk

[ -f firma.keystore ] || keytool -genkeypair -keystore firma.keystore -alias audiowire \
  -keyalg RSA -keysize 2048 -validity 10000 -storepass audiowire -keypass audiowire \
  -dname "CN=AudioWire" >/dev/null 2>&1
apksigner sign --ks firma.keystore --ks-pass pass:audiowire --key-pass pass:audiowire \
  --min-sdk-version 24 --out AudioWire.apk $B/alineado.apk
apksigner verify --min-sdk-version 24 AudioWire.apk
rm -rf $B
echo "Listo: AudioWire.apk"
