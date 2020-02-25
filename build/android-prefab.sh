#!/bin/bash
# expected to run from "${BASEDIR}/android"

export FFMPEG_VERSION=`sed -e "s/\.git//" "${BASEDIR}/src/ffmpeg/RELEASE"` # used in build.gradle

mkdir -p "build/publications/ffmpeg_aar" 1>>${BASEDIR}/build.log 2>&1

mkdir -p "build/publications/ffmpeg_aar/META-INF" 1>>${BASEDIR}/build.log 2>&1
cp "${BASEDIR}/src/ffmpeg/COPYING.LGPLv2.1" "build/publications/ffmpeg_aar/META-INF/LICENSE" 1>>${BASEDIR}/build.log 2>&1

mkdir -p "build/publications/ffmpeg_aar/prefab" 1>>${BASEDIR}/build.log 2>&1
echo '{"name":"ffmpeg","schema_version":1,"dependencies":[],"version":"'${FFMPEG_VERSION}'"}' >"build/publications/ffmpeg_aar/prefab/prefab.json"

prefab_abi_subdir() { #TODO: wrap each .so as a separate module
  local SRC="${BASEDIR}/prebuilt/android-$1/ffmpeg"
  local ABI=$2
  local DEST="build/publications/ffmpeg_aar/prefab/modules/ffmpeg/libs/android.${ABI}"

  if [ -d "${SRC}/lib" ]; then
    mkdir -p ${DEST} 1>>${BASEDIR}/build.log 2>&1
    cp -R ${SRC}/include ${DEST} 1>>${BASEDIR}/build.log 2>&1
    cp ${SRC}/lib/*.so ${DEST} 1>>${BASEDIR}/build.log 2>&1
    echo '{"abi":"'$ABI'","api":'$API',"ndk":20,"stl":"none"}' >${DEST}/abi.json
  fi
}

prefab_abi_subdir "arm"   "armeabi-v7a"
prefab_abi_subdir "arm64" "arm64-v8a"
prefab_abi_subdir "x86"    "x86"
prefab_abi_subdir "x86_64" "x86_64"

echo '{"export_libraries":[],"library_name":null,"android":{"export_libraries":null,"library_name":null}}' >"build/publications/ffmpeg_aar/prefab/modules/ffmpeg/module.json"

cd build/publications/ffmpeg_aar 1>>${BASEDIR}/build.log 2>&1
zip -r ../ffmpeg.aar * 1>>${BASEDIR}/build.log 2>&1
cd - 1>>${BASEDIR}/build.log 2>&1
rm -rf build/publications/ffmpeg_aar

./gradlew publish 1>>${BASEDIR}/build.log 2>&1

echo -e "\nINFO: Completed publish at "$(date)"\n" 1>>${BASEDIR}/build.log 2>&1