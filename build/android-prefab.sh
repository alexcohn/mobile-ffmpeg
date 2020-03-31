#!/bin/bash
cd "${BASEDIR}/android"

readonly FFMPEG_VERSION=$(sed -e "s/\.git//" "${BASEDIR}/src/ffmpeg/RELEASE")
export FFMPEG_VERSION # used in build.gradle
readonly NDK_VERSION=20 # see https://github.com/google/prefab/issues/74

mkdir -p "build/publications/ffmpeg_aar" 1>>${BASEDIR}/build.log 2>&1

echo '<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.android.ndk.thirdparty.ffmpeg" android:versionCode="1" android:versionName="1.0">' >build/publications/ffmpeg_aar/AndroidManifest.xml
echo '  <uses-sdk android:minSdkVersion="'${API}'" android:targetSdkVersion="29"/>' >>build/publications/ffmpeg_aar/AndroidManifest.xml
echo '</manifest>' >>build/publications/ffmpeg_aar/AndroidManifest.xml

mkdir -p "build/publications/ffmpeg_aar/META-INF" 1>>${BASEDIR}/build.log 2>&1
cp "${BASEDIR}/src/ffmpeg/COPYING.LGPLv2.1" "build/publications/ffmpeg_aar/META-INF" 1>>${BASEDIR}/build.log 2>&1

mkdir -p "build/publications/ffmpeg_aar/prefab" 1>>${BASEDIR}/build.log 2>&1
echo -n '{"name":"ffmpeg","schema_version":1,"dependencies":[],"version":"'${FFMPEG_VERSION}'"}' >"build/publications/ffmpeg_aar/prefab/prefab.json"

prefab_abi_subdir() {
  local SRC="${BASEDIR}/prebuilt/android-$1/ffmpeg"
  local ABI=$2

  for MODULE in libavcodec libavdevice libavfilter libavformat libswresample libswscale libavutil; do
    local DEST="build/publications/ffmpeg_aar/prefab/modules/${MODULE}/libs/android.${ABI}"

    if [ -f "${SRC}/lib/${MODULE}.so" ]; then
      mkdir -p "${DEST}" 1>>${BASEDIR}/build.log 2>&1
      cp "${SRC}/lib/${MODULE}.so" "${DEST}" 1>>${BASEDIR}/build.log 2>&1
      echo -n '{"abi":"'${ABI}'","api":'${API}',"ndk":'${NDK_VERSION}',"stl":"none"}' >"${DEST}/abi.json" 2>>${BASEDIR}/build.log
      echo -n '{"export_libraries":[],"library_name":"'${MODULE}'"}' >"${DEST}/../../module.json" 2>>${BASEDIR}/build.log
    fi
  done
  cp -R ${SRC}/include ${DEST} 1>>${BASEDIR}/build.log 2>&1 # to last module
}

prefab_abi_subdir "arm"    "armeabi-v7a"
prefab_abi_subdir "arm64"  "arm64-v8a"
prefab_abi_subdir "x86"    "x86"
prefab_abi_subdir "x86_64" "x86_64"

(cd build/publications/ffmpeg_aar && zip -r ../ffmpeg.aar * 1>>${BASEDIR}/build.log 2>&1)
rm -rf build/publications/ffmpeg_aar

#./gradlew publish 1>>${BASEDIR}/build.log 2>&1

echo -e "\nINFO: Completed publish at $(date)\n" 1>>${BASEDIR}/build.log 2>&1