@echo off
rem Bouwt GPUINT voor Android: uit\GPUINT.apk. Zonder Gradle: de C-compiler van de NDK, dan inpakken en ondertekenen.
rem Nodig: Android SDK met NDK 25.1.8937393, build-tools 34.0.0 en platform android-34, en Java.
rem De iconen (res\mipmap-*) maakt bouw.bat in de map erboven (maak_icoon.c).
cd /d "%~dp0"
if "%ANDROID_SDK%"=="" set ANDROID_SDK=%LOCALAPPDATA%\Android\Sdk
set NDK=%ANDROID_SDK%\ndk\25.1.8937393\toolchains\llvm\prebuilt\windows-x86_64\bin
set BT=%ANDROID_SDK%\build-tools\34.0.0
if not exist res\mipmap-xxxhdpi\ic_launcher.png ( echo Eerst bouw.bat in de map erboven draaien: dat maakt de iconen. & exit /b 1 )
if exist uit rmdir /s /q uit
mkdir uit\lib\arm64-v8a || exit /b 1
call "%NDK%\aarch64-linux-android26-clang.cmd" -O2 -Wall -Wno-unused-function -shared -fPIC -o uit\lib\arm64-v8a\libgpuint.so gpuint.c -landroid -lm -Wl,-z,max-page-size=16384 || exit /b 1
"%BT%\aapt2.exe" compile --dir res -o uit\res.zip || exit /b 1
"%BT%\aapt2.exe" link -o uit\los.apk --manifest AndroidManifest.xml -I "%ANDROID_SDK%\platforms\android-34\android.jar" --min-sdk-version 26 --target-sdk-version 34 --version-code 1 --version-name 0.1 uit\res.zip || exit /b 1
cd uit
"%BT%\aapt.exe" add los.apk lib/arm64-v8a/libgpuint.so >nul || exit /b 1
cd ..
"%BT%\zipalign.exe" -f -p 4 uit\los.apk uit\lijn.apk || exit /b 1
rem Ondertekend met de proefsleutel van Android (alleen voor eigen gebruik en testen).
call "%BT%\apksigner.bat" sign --ks "%USERPROFILE%\.android\debug.keystore" --ks-pass pass:android --out uit\GPUINT.apk uit\lijn.apk || exit /b 1
del uit\los.apk uit\lijn.apk uit\res.zip uit\GPUINT.apk.idsig 2>nul
echo Klaar: uit\GPUINT.apk
