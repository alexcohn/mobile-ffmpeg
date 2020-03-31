APP_OPTIM := release
APP_CFLAGS := -O3 -DANDROID $(LTS_BUILD_FLAG) -DMOBILE_FFMPEG_BUILD_DATE=$(BUILD_DATE) -Wall -Wno-deprecated-declarations -Wno-pointer-sign -Wno-switch -Wno-unused-result -Wno-unused-variable
APP_LDFLAGS := -Wl,--hash-style=both
APP_STL := c++_shared