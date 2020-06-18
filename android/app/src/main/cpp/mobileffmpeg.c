/*
 * Copyright (c) 2018 Taner Sener
 *
 * This file is part of MobileFFmpeg.
 *
 * MobileFFmpeg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MobileFFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with MobileFFmpeg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * CHANGES 08.2019
 * --------------------------------------------------------
 * - lastCommandOutput methods introduced
 * - AV_LOG_STDERR introduced
 *
 * CHANGES 04.2019
 * --------------------------------------------------------
 * - setNativeEnvironmentVariable method added
 *
 * CHANGES 02.2019
 * --------------------------------------------------------
 * - JavaVM registered via av_jni_set_java_vm()
 * - registerNewNativeFFmpegPipe() method added
 *
 * CHANGES 10.2018
 * --------------------------------------------------------
 * - getBuildConf method added
 *
 * CHANGES 09.2018
 * --------------------------------------------------------
 * - Merged with mobileffmpeg_config
 *
 * CHANGES 08.2018
 * --------------------------------------------------------
 * - Copied methods with avutil_log_ prefix from libavutil/log.c
 */

#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "config.h"
#include "libavcodec/jni.h"
#include "libavutil/bprint.h"
#include "fftools_ffmpeg.h"
#include "mobileffmpeg.h"
#include "mobileffprobe.h"

/** Callback data structure */
struct CallbackData {
  int type;                 // 1 (log callback) or 2 (statistics callback)

  int logLevel;             // log level
  AVBPrint logData;         // log data

  int statisticsFrameNumber;        // statistics frame number
  float statisticsFps;              // statistics fps
  float statisticsQuality;          // statistics quality
  int64_t statisticsSize;           // statistics size
  int statisticsTime;               // statistics time
  double statisticsBitrate;         // statistics bitrate
  double statisticsSpeed;           // statistics speed

  struct CallbackData *next;
};

/** Redirection control variables */
pthread_mutex_t lockMutex;
pthread_mutex_t monitorMutex;
pthread_cond_t monitorCondition;

/** Last command output variables */
pthread_mutex_t logMutex;
static AVBPrint lastCommandOutput;

pthread_t callbackThread;
int redirectionEnabled;

struct CallbackData *callbackDataHead;
struct CallbackData *callbackDataTail;

/** Global reference to the virtual machine running */
static JavaVM *globalVm;

/** Global application context */
static jobject *appContext;

/** Global reference of Config class in Java */
static jclass configClass;

/** Global reference of log redirection method in Java */
static jmethodID logMethod;

/** Global reference of statistics redirection method in Java */
static jmethodID statisticsMethod;

/** Global reference of String class in Java */
static jclass stringClass;

/** Global reference of String constructor in Java */
static jmethodID stringConstructor;

/** Full name of the Config class */
const char *configClassName = "com/arthenica/mobileffmpeg/Config";

/** Full name of String class */
const char *stringClassName = "java/lang/String";

/** Prototypes of native functions defined by Config class. */
JNINativeMethod configMethods[] = {
    {"enableNativeRedirection", "()V", (void*) Java_com_arthenica_mobileffmpeg_Config_enableNativeRedirection},
    {"disableNativeRedirection", "()V", (void*) Java_com_arthenica_mobileffmpeg_Config_disableNativeRedirection},
    {"setNativeLogLevel", "(I)V", (void*) Java_com_arthenica_mobileffmpeg_Config_setNativeLogLevel},
    {"getNativeLogLevel", "()I", (void*) Java_com_arthenica_mobileffmpeg_Config_getNativeLogLevel},
    {"getNativeFFmpegVersion", "()Ljava/lang/String;", (void*) Java_com_arthenica_mobileffmpeg_Config_getNativeFFmpegVersion},
    {"getNativeVersion", "()Ljava/lang/String;", (void*) Java_com_arthenica_mobileffmpeg_Config_getNativeVersion},
    {"nativeFFmpegExecute", "([Ljava/lang/String;)I", (void*) Java_com_arthenica_mobileffmpeg_Config_nativeFFmpegExecute},
    {"nativeFFmpegCancel", "()V", (void*) Java_com_arthenica_mobileffmpeg_Config_nativeFFmpegCancel},
    {"nativeFFprobeExecute", "([Ljava/lang/String;)I", (void*) Java_com_arthenica_mobileffmpeg_Config_nativeFFprobeExecute},
    {"registerNewNativeFFmpegPipe", "(Ljava/lang/String;)I", (void*) Java_com_arthenica_mobileffmpeg_Config_registerNewNativeFFmpegPipe},
    {"getNativeBuildDate", "()Ljava/lang/String;", (void*) Java_com_arthenica_mobileffmpeg_Config_getNativeBuildDate},
    {"setNativeEnvironmentVariable", "(Ljava/lang/String;Ljava/lang/String;)I", (void*) Java_com_arthenica_mobileffmpeg_Config_setNativeEnvironmentVariable}
};

/** Forward declaration for function defined in fftools_ffmpeg.c */
int ffmpeg_execute(int argc, char **argv);

/** DEFINES LINE SIZE USED FOR LOGGING */
#define LOG_LINE_SIZE 1024

static const char *avutil_log_get_level_str(int level) {
    switch (level) {
    case AV_LOG_STDERR:
        return "stderr";
    case AV_LOG_QUIET:
        return "quiet";
    case AV_LOG_DEBUG:
        return "debug";
    case AV_LOG_VERBOSE:
        return "verbose";
    case AV_LOG_INFO:
        return "info";
    case AV_LOG_WARNING:
        return "warning";
    case AV_LOG_ERROR:
        return "error";
    case AV_LOG_FATAL:
        return "fatal";
    case AV_LOG_PANIC:
        return "panic";
    default:
        return "";
    }
}

static void avutil_log_format_line(void *avcl, int level, const char *fmt, va_list vl, AVBPrint part[4], int *print_prefix) {
    int flags = av_log_get_flags();
    AVClass* avc = avcl ? *(AVClass **) avcl : NULL;
    av_bprint_init(part+0, 0, 1);
    av_bprint_init(part+1, 0, 1);
    av_bprint_init(part+2, 0, 1);
    av_bprint_init(part+3, 0, 65536);

    if (*print_prefix && avc) {
        if (avc->parent_log_context_offset) {
            AVClass** parent = *(AVClass ***) (((uint8_t *) avcl) +
                                   avc->parent_log_context_offset);
            if (parent && *parent) {
                av_bprintf(part+0, "[%s @ %p] ",
                         (*parent)->item_name(parent), parent);
            }
        }
        av_bprintf(part+1, "[%s @ %p] ",
                 avc->item_name(avcl), avcl);
    }

    if (*print_prefix && (level > AV_LOG_QUIET) && (flags & AV_LOG_PRINT_LEVEL))
        av_bprintf(part+2, "[%s] ", avutil_log_get_level_str(level));

    av_vbprintf(part+3, fmt, vl);

    if(*part[0].str || *part[1].str || *part[2].str || *part[3].str) {
        char lastc = part[3].len && part[3].len <= part[3].size ? part[3].str[part[3].len - 1] : 0;
        *print_prefix = lastc == '\n' || lastc == '\r';
    }
}

static void avutil_log_sanitize(uint8_t *line) {
    while(*line){
        if(*line < 0x08 || (*line > 0x0D && *line < 0x20))
            *line='?';
        line++;
    }
}

void mutexInit() {
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE_NP);

    pthread_mutex_init(&lockMutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
}

void monitorInit() {
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE_NP);

    pthread_condattr_t cattributes;
    pthread_condattr_init(&cattributes);
    pthread_condattr_setpshared(&cattributes, PTHREAD_PROCESS_PRIVATE);

    pthread_mutex_init(&monitorMutex, &attributes);
    pthread_mutexattr_destroy(&attributes);

    pthread_cond_init(&monitorCondition, &cattributes);
    pthread_condattr_destroy(&cattributes);
}

void logInit() {
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE_NP);

    pthread_mutex_init(&logMutex, &attributes);
    pthread_mutexattr_destroy(&attributes);

    av_bprint_init(&lastCommandOutput, 0, AV_BPRINT_SIZE_UNLIMITED);
}

void mutexUnInit() {
    pthread_mutex_destroy(&lockMutex);
}

void monitorUnInit() {
    pthread_mutex_destroy(&monitorMutex);
    pthread_cond_destroy(&monitorCondition);
}

void logUnInit() {
    pthread_mutex_destroy(&logMutex);
}

void mutexLock() {
    pthread_mutex_lock(&lockMutex);
}

void lastCommandOutputLock() {
    pthread_mutex_lock(&logMutex);
}

void mutexUnlock() {
    pthread_mutex_unlock(&lockMutex);
}

void lastCommandOutputUnlock() {
    pthread_mutex_unlock(&logMutex);
}

void clearLastCommandOutput() {
    lastCommandOutputLock();
    av_bprint_clear(&lastCommandOutput);
    lastCommandOutputUnlock();
}

void appendLastCommandOutput(AVBPrint *logMessage) {
    if (logMessage->len <= 0) {
        return;
    }

    lastCommandOutputLock();
    av_bprintf(&lastCommandOutput, "%s", logMessage->str);
    lastCommandOutputUnlock();
}

void monitorWait(int milliSeconds) {
    struct timeval tp;
    struct timespec ts;
    int rc;

    rc = gettimeofday(&tp, NULL);
    if (rc) {
        return;
    }

    ts.tv_sec  = tp.tv_sec;
    ts.tv_nsec = tp.tv_usec * 1000;
    ts.tv_sec += milliSeconds / 1000;
    ts.tv_nsec += (milliSeconds % 1000)*1000000;

    pthread_mutex_lock(&monitorMutex);
    pthread_cond_timedwait(&monitorCondition, &monitorMutex, &ts);
    pthread_mutex_unlock(&monitorMutex);
}

void monitorNotify() {
    pthread_mutex_lock(&monitorMutex);
    pthread_cond_signal(&monitorCondition);
    pthread_mutex_unlock(&monitorMutex);
}

/**
 * Adds log data to the end of callback data list.
 *
 * @param level log level
 * @param data log data
 */
void logCallbackDataAdd(int level, AVBPrint *data) {

    // CREATE DATA STRUCT FIRST
    struct CallbackData *newData = (struct CallbackData*)av_malloc(sizeof(struct CallbackData));
    newData->type = 1;
    newData->logLevel = level;
    av_bprint_init(&newData->logData, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&newData->logData, "%s", data->str);
    newData->next = NULL;

    mutexLock();

    // INSERT IT TO THE END OF QUEUE
    if (callbackDataTail == NULL) {
        callbackDataTail = newData;

        if (callbackDataHead != NULL) {
            LOGE("Dangling callback data head detected. This can cause memory leak.");
        } else {
            callbackDataHead = newData;
        }
    } else {
        struct CallbackData *oldTail = callbackDataTail;
        oldTail->next = newData;

        callbackDataTail = newData;
    }

    mutexUnlock();

    monitorNotify();
}

/**
 * Adds statistics data to the end of callback data list.
 */
void statisticsCallbackDataAdd(int frameNumber, float fps, float quality, int64_t size, int time, double bitrate, double speed) {

    // CREATE DATA STRUCT FIRST
    struct CallbackData *newData = (struct CallbackData*)av_malloc(sizeof(struct CallbackData));
    newData->type = 2;
    newData->statisticsFrameNumber = frameNumber;
    newData->statisticsFps = fps;
    newData->statisticsQuality = quality;
    newData->statisticsSize = size;
    newData->statisticsTime = time;
    newData->statisticsBitrate = bitrate;
    newData->statisticsSpeed = speed;

    newData->next = NULL;

    mutexLock();

    // INSERT IT TO THE END OF QUEUE
    if (callbackDataTail == NULL) {
        callbackDataTail = newData;

        if (callbackDataHead != NULL) {
            LOGE("Dangling callback data head detected. This can cause memory leak.");
        } else {
            callbackDataHead = newData;
        }
    } else {
        struct CallbackData *oldTail = callbackDataTail;
        oldTail->next = newData;

        callbackDataTail = newData;
    }

    mutexUnlock();

    monitorNotify();
}

/**
 * Removes head of callback data list.
 */
struct CallbackData *callbackDataRemove() {
    struct CallbackData *currentData;

    mutexLock();

    if (callbackDataHead == NULL) {
        currentData = NULL;
    } else {
        currentData = callbackDataHead;

        struct CallbackData *nextHead = currentData->next;
        if (nextHead == NULL) {
            if (callbackDataHead != callbackDataTail) {
                LOGE("Head and tail callback data pointers do not match for single callback data element. This can cause memory leak.");
            } else {
                callbackDataTail = NULL;
            }
            callbackDataHead = NULL;

        } else {
            callbackDataHead = nextHead;
        }
    }

    mutexUnlock();

    return currentData;
}

/**
 * Callback function for FFmpeg logs.
 *
 * @param ptr pointer to AVClass struct
 * @param level log level
 * @param format format string
 * @param vargs arguments
 */
void mobileffmpeg_log_callback_function(void *ptr, int level, const char* format, va_list vargs) {
    AVBPrint fullLine;
    AVBPrint part[4];
    int print_prefix = 1;

    if (level >= 0) {
        level &= 0xff;
    }
    int activeLogLevel = av_log_get_level();

    // AV_LOG_STDERR logs are always redirected
    if ((activeLogLevel == AV_LOG_QUIET && level != AV_LOG_STDERR) || (level > activeLogLevel)) {
        return;
    }

    av_bprint_init(&fullLine, 0, AV_BPRINT_SIZE_UNLIMITED);

    avutil_log_format_line(ptr, level, format, vargs, part, &print_prefix);
    avutil_log_sanitize(part[0].str);
    avutil_log_sanitize(part[1].str);
    avutil_log_sanitize(part[2].str);
    avutil_log_sanitize(part[3].str);

    // COMBINE ALL 4 LOG PARTS
    av_bprintf(&fullLine, "%s%s%s%s", part[0].str, part[1].str, part[2].str, part[3].str);

    if (fullLine.len > 0) {
        logCallbackDataAdd(level, &fullLine);
        appendLastCommandOutput(&fullLine);
    }

    av_bprint_finalize(part, NULL);
    av_bprint_finalize(part+1, NULL);
    av_bprint_finalize(part+2, NULL);
    av_bprint_finalize(part+3, NULL);
    av_bprint_finalize(&fullLine, NULL);
}

/**
 * Callback function for FFmpeg statistics.
 *
 * @param frameNumber last processed frame number
 * @param fps frames processed per second
 * @param quality quality of the output stream (video only)
 * @param size size in bytes
 * @param time processed output duration
 * @param bitrate output bit rate in kbits/s
 * @param speed processing speed = processed duration / operation duration
 */
void mobileffmpeg_statistics_callback_function(int frameNumber, float fps, float quality, int64_t size, int time, double bitrate, double speed) {
    statisticsCallbackDataAdd(frameNumber, fps, quality, size, time, bitrate, speed);
}

/**
 * Forwards callback messages to Java classes.
 */
void *callbackThreadFunction() {
    JNIEnv *env;
    jint getEnvRc = (*globalVm)->GetEnv(globalVm, (void**) &env, JNI_VERSION_1_6);
    if (getEnvRc != JNI_OK) {
        if (getEnvRc != JNI_EDETACHED) {
            LOGE("Callback thread failed to GetEnv for class %s with rc %d.\n", configClassName, getEnvRc);
            return NULL;
        }

        if ((*globalVm)->AttachCurrentThread(globalVm, &env, NULL) != 0) {
            LOGE("Callback thread failed to AttachCurrentThread for class %s.\n", configClassName);
            return NULL;
        }
    }

    LOGD("Callback thread started.\n");

    while(redirectionEnabled) {

        struct CallbackData *callbackData = callbackDataRemove();
        if (callbackData != NULL) {
            if (callbackData->type == 1) {

                // LOG CALLBACK

                int size = callbackData->logData.len;

                jbyteArray byteArray = (jbyteArray) (*env)->NewByteArray(env, size);
                (*env)->SetByteArrayRegion(env, byteArray, 0, size, callbackData->logData.str);
                (*env)->CallStaticVoidMethod(env, configClass, logMethod, callbackData->logLevel, byteArray);
                (*env)->DeleteLocalRef(env, byteArray);

                // CLEAN LOG DATA
                av_bprint_finalize(&callbackData->logData, NULL);

            } else {

                // STATISTICS CALLBACK

                (*env)->CallStaticVoidMethod(env, configClass, statisticsMethod,
                    callbackData->statisticsFrameNumber, callbackData->statisticsFps,
                    callbackData->statisticsQuality, callbackData->statisticsSize,
                    callbackData->statisticsTime, callbackData->statisticsBitrate,
                    callbackData->statisticsSpeed);

            }

            // CLEAN STRUCT
            callbackData->next = NULL;
            av_free(callbackData);

        } else {
            monitorWait(100);
        }
    }

    (*globalVm)->DetachCurrentThread(globalVm);

    LOGD("Callback thread stopped.\n");

    return NULL;
}

/**
 * Kudos to Harlan Chen, https://stackoverflow.com/a/46871051/192373
 */
static jobject get_global_context(JNIEnv *env) {
    jclass activityThread = (*env)->FindClass(env, "android/app/ActivityThread");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        LOGE("get_global_context FindClass %s exception", "android/app/ActivityThread");
        return NULL;
    }
    jmethodID currentActivityThread = (*env)->GetStaticMethodID(env, activityThread, "currentActivityThread", "()Landroid/app/ActivityThread;");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        LOGE("get_global_context find %s exception", "currentActivityThread");
        return NULL;
    }
    jobject at = (*env)->CallStaticObjectMethod(env, activityThread, currentActivityThread);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        LOGE("get_global_context %s %s exception", "android/app/ActivityThread", "currentActivityThread");
        return NULL;
    }
    jmethodID getApplication = (*env)->GetMethodID(env, activityThread, "getApplication", "()Landroid/app/Application;");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        LOGE("get_global_context find %s exception", "getApplication");
        return NULL;
    }
    jobject context = (*env)->CallObjectMethod(env, at, getApplication);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        LOGE("get_global_context %s exception", "getApplication");
        return NULL;
    }

    return (*env)->NewGlobalRef(env, context);
    // TODO: release local refs
}

/**
 * Called when 'mobileffmpeg' native library is loaded.
 *
 * @param vm pointer to the running virtual machine
 * @param reserved reserved
 * @return JNI version needed by 'mobileffmpeg' library
 */
jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void**)(&env), JNI_VERSION_1_6) != JNI_OK) {
        LOGE("OnLoad failed to GetEnv for class %s.\n", configClassName);
        return JNI_FALSE;
    }

    jclass localConfigClass = (*env)->FindClass(env, configClassName);
    if (localConfigClass == NULL) {
        LOGE("OnLoad failed to FindClass %s.\n", configClassName);
        return JNI_FALSE;
    }

    if ((*env)->RegisterNatives(env, localConfigClass, configMethods, 12) < 0) {
        LOGE("OnLoad failed to RegisterNatives for class %s.\n", configClassName);
        return JNI_FALSE;
    }

    jclass localStringClass = (*env)->FindClass(env, stringClassName);
    if (localStringClass == NULL) {
        LOGE("OnLoad failed to FindClass %s.\n", stringClassName);
        return JNI_FALSE;
    }

    (*env)->GetJavaVM(env, &globalVm);

    logMethod = (*env)->GetStaticMethodID(env, localConfigClass, "log", "(I[B)V");
    if (logMethod == NULL) {
        LOGE("OnLoad thread failed to GetStaticMethodID for %s.\n", "log");
        (*globalVm)->DetachCurrentThread(globalVm);
        return JNI_FALSE;
    }

    statisticsMethod = (*env)->GetStaticMethodID(env, localConfigClass, "statistics", "(IFFJIDD)V");
    if (logMethod == NULL) {
        LOGE("OnLoad thread failed to GetStaticMethodID for %s.\n", "statistics");
        (*globalVm)->DetachCurrentThread(globalVm);
        return JNI_FALSE;
    }

    stringConstructor = (*env)->GetMethodID(env, localStringClass, "<init>", "([BLjava/lang/String;)V");
    if (stringConstructor == NULL) {
        LOGE("OnLoad thread failed to GetMethodID for %s.\n", "<init>");
        (*globalVm)->DetachCurrentThread(globalVm);
        return JNI_FALSE;
    }

    av_jni_set_java_vm(vm, NULL);

    configClass = (jclass) ((*env)->NewGlobalRef(env, localConfigClass));
    stringClass = (jclass) ((*env)->NewGlobalRef(env, localStringClass));

    redirectionEnabled = 0;

    callbackDataHead = NULL;
    callbackDataTail = NULL;

    mutexInit();
    monitorInit();
    logInit();

    appContext = get_global_context(env);
    LOGI("set appContext=%p", appContext);

    return JNI_VERSION_1_6;
}

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

static jclass    android_net_Uri = NULL; /* global ref */

static jmethodID android_net_Uri_parse = NULL;
static jmethodID android_content_Context_getContentResolver = NULL;
static jmethodID android_content_ContentResolver_openFileDescriptor = NULL;
static jmethodID android_content_ContentResolver_query = NULL;
static jmethodID android_os_ParcelFileDescriptor_getFd = NULL;
static jmethodID android_database_Cursor_moveToFirst = NULL;
static jmethodID android_database_Cursor_getString = NULL;
static jmethodID android_database_Cursor_getColumnIndex = NULL;
static jmethodID android_database_Cursor_close = NULL;

static jstring DISPLAY_NAME = NULL;
static jobject android_content_ContentResolver_instance = NULL;  /* global ref */

#define JNI_CHECK_EXTRA
#define JNI_CHECK(j_object, format, ...) { \
    if ((*env)->ExceptionCheck(env)) { \
        (*env)->ExceptionClear(env); \
        LOGE("%s:%d JNI exception, cannot get " #j_object " from " format, __PRETTY_FUNCTION__, __LINE__, __VA_ARGS__); \
        JNI_CHECK_EXTRA; \
        (*env)->PopLocalFrame(env, NULL); \
        return JNI_ERR; \
    } \
    if (!j_object) { \
        LOGE("%s:%d cannot get" #j_object " from " format, __PRETTY_FUNCTION__, __LINE__, __VA_ARGS__); \
        JNI_CHECK_EXTRA; \
        (*env)->PopLocalFrame(env, NULL); \
        return JNI_ERR; \
    } \
}

static int get_static_method_id(JNIEnv *env, const char *class_name, const char *name, const char *signature, jclass *global_class_reference, jmethodID *method_id) {
    jclass class_reference = (*env)->FindClass(env, class_name);
    JNI_CHECK(class_reference, "%s", class_name);
    *global_class_reference = (*env)->NewGlobalRef(env, class_reference);
    JNI_CHECK(*global_class_reference, "%p %s", class_reference, class_name);
    (*env)->DeleteLocalRef(env, class_reference);

    *method_id = (*env)->GetStaticMethodID(env, *global_class_reference, name, signature);
    JNI_CHECK(*method_id, "%p class %s static method %s for %s", *global_class_reference, class_name, name, signature);
    return JNI_OK;
}

static int get_method_id(JNIEnv *env, const char *class_name, const char *name, const char *signature, jmethodID *method_id) {
    jclass class_reference = (*env)->FindClass(env, class_name);
    JNI_CHECK(class_reference, "%s", class_name);

    *method_id = (*env)->GetMethodID(env, class_reference, name, signature);
    JNI_CHECK(*method_id, "%p class %s method %s for %s", class_reference, class_name, name, signature);
    (*env)->DeleteLocalRef(env, class_reference);
    return JNI_OK;
}

int match_ext_from_content(const char *content, const char *extensions);
int get_fd_from_content(const char *content, int access) {

    int fd = -1;

    JNIEnv *env;
    int ret = (*globalVm)->GetEnv(globalVm, (void **)&env, JNI_VERSION_1_6);
    if (!env || ret != JNI_OK) {
        LOGE("%s:%d cannot get env", __PRETTY_FUNCTION__, __LINE__);
        return -1;
    }

    ret = (*env)->PushLocalFrame(env, 10);

    if (!android_net_Uri) {
        if (get_static_method_id(env, "android/net/Uri", "parse", "(Ljava/lang/String;)Landroid/net/Uri;", &android_net_Uri, &android_net_Uri_parse) != JNI_OK)
            return JNI_ERR;

        if (get_method_id(env, "android/content/Context", "getContentResolver", "()Landroid/content/ContentResolver;", &android_content_Context_getContentResolver) != JNI_OK)
            return JNI_ERR;

        if (get_method_id(env, "android/content/ContentResolver", "openFileDescriptor", "(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;", &android_content_ContentResolver_openFileDescriptor) != JNI_OK)
            return JNI_ERR;

        if (get_method_id(env, "android/content/ContentResolver", "query", "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;", &android_content_ContentResolver_query) != JNI_OK)
            return JNI_ERR;

        if (get_method_id(env, "android/database/Cursor", "moveToFirst", "()Z", &android_database_Cursor_moveToFirst) != JNI_OK)
            return JNI_ERR;

        if (get_method_id(env, "android/database/Cursor", "getString", "(I)Ljava/lang/String;", &android_database_Cursor_getString) != JNI_OK)
            return JNI_ERR;

        if (get_method_id(env, "android/database/Cursor", "getColumnIndex", "(Ljava/lang/String;)I", &android_database_Cursor_getColumnIndex) != JNI_OK)
            return JNI_ERR;

        if (get_method_id(env, "android/database/Cursor", "close", "()V", &android_database_Cursor_close) != JNI_OK)
            return JNI_ERR;

        if (get_method_id(env, "android/os/ParcelFileDescriptor", "getFd", "()I", &android_os_ParcelFileDescriptor_getFd) != JNI_OK)
            return JNI_ERR;

        DISPLAY_NAME = (*env)->NewStringUTF(env, "_display_name"); // DocumentsContract.Document.COLUMN_DISPLAY_NAME
        JNI_CHECK(DISPLAY_NAME, "DocumentsContract.Document.COLUMN_DISPLAY_NAME=%s", "_display_name");
    }

    const char *fmode = "r";
    if (access & (O_WRONLY | O_RDWR)) {
        fmode = "w";
    }

    LOGI("get_fd_from_content" " \"%s\" fd from %s", fmode, content);

    jstring uriString = (*env)->NewStringUTF(env, content);
    JNI_CHECK(uriString, "%s", content);

    jstring fmodeString = (*env)->NewStringUTF(env, fmode);
    JNI_CHECK(fmodeString, "%s", fmode);

    jobject uri = (*env)->CallStaticObjectMethod(env, android_net_Uri, android_net_Uri_parse, uriString);
    JNI_CHECK(uri, "%p", uriString);

    if (android_content_ContentResolver_instance == NULL) {
        jobject contentResolver = (*env)->CallObjectMethod(env, appContext, android_content_Context_getContentResolver);
        JNI_CHECK(contentResolver, "%p", appContext);
        android_content_ContentResolver_instance = (*env)->NewGlobalRef(env, contentResolver);
        JNI_CHECK(android_content_ContentResolver_instance, "%p", contentResolver);
    }

    jobject parcelFileDescriptor = (*env)->CallObjectMethod(env, android_content_ContentResolver_instance, android_content_ContentResolver_openFileDescriptor, uri, fmodeString);
    JNI_CHECK(parcelFileDescriptor, "%p (%s, %s)", android_content_ContentResolver_instance, content, fmode);

    fd = (*env)->CallIntMethod(env, parcelFileDescriptor, android_os_ParcelFileDescriptor_getFd);
    JNI_CHECK((fd >= 0), "%p", parcelFileDescriptor);

    LOGI("match_ext_from_content: %d", match_ext_from_content(content, "mp4"));

    (*env)->PopLocalFrame(env, NULL);
    return dup(fd);
}

/* based on https://stackoverflow.com/a/25005243/192373

  Cursor cursor = getContentResolver().query(uri, null, null, null, null);
    try {
      if (cursor != null && cursor.moveToFirst()) {
        result = cursor.getString(cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DISPLAY_NAME));
      }
    } finally {
      cursor.close();
    }
*/

static int get_filename_from_content(const char *content, char filename[]) {

    if (!android_content_ContentResolver_instance) {
        LOGE("%s:%d have not initialized android.content.ContentResolver instance", __PRETTY_FUNCTION__, __LINE__);
        return JNI_ERR;
    }

    if (!android_content_ContentResolver_openFileDescriptor) {
        LOGE("%s:%d have not initialized android.content.ContentResolver instance", __PRETTY_FUNCTION__, __LINE__);
        return JNI_ERR;
    }

    JNIEnv *env;
    int ret = (*globalVm)->GetEnv(globalVm, (void **)&env, JNI_VERSION_1_6);
    if (!env || ret != JNI_OK) {
        LOGE("%s:%d cannot get env", __PRETTY_FUNCTION__, __LINE__);
        return JNI_ERR;
    }

    ret = (*env)->PushLocalFrame(env, 10);

    jstring uriString = (*env)->NewStringUTF(env, content);
    JNI_CHECK(uriString, "%s", content);

    jobject uri = (*env)->CallStaticObjectMethod(env, android_net_Uri, android_net_Uri_parse, uriString);
    JNI_CHECK(uri, "%p", uriString);

    jobject cursor = (*env)->CallObjectMethod(env, android_content_ContentResolver_instance, android_content_ContentResolver_query, uri, NULL, NULL, NULL, NULL);
    JNI_CHECK(cursor, "%p (%s, null, null, null, null)", android_content_ContentResolver_instance, content);

#undef  JNI_CHECK_EXTRA
#define JNI_CHECK_EXTRA (*env)->CallVoidMethod(env, cursor, android_database_Cursor_close)

    jboolean move_res = (*env)->CallBooleanMethod(env, cursor, android_database_Cursor_moveToFirst);
    JNI_CHECK(move_res, "%p moveToFirst()", cursor);

    jint column_idx = (*env)->CallIntMethod(env, cursor, android_database_Cursor_getColumnIndex, DISPLAY_NAME);
    JNI_CHECK((column_idx >= 0), "%p getColumnIndex(DISPLAY_NAME)", cursor);

    jstring j_str = (jstring)(*env)->CallObjectMethod(env, cursor, android_database_Cursor_getString, column_idx);
    JNI_CHECK(j_str, "%p getString (%d)", cursor, column_idx);

#undef JNI_CHECK_EXTRA
#define JNI_CHECK_EXTRA

    (*env)->CallVoidMethod(env, cursor, android_database_Cursor_close);

    const char *c_str = (*env)->GetStringUTFChars(env, j_str, 0);
    JNI_CHECK(c_str, "GetStringUTFChars %p", j_str);

    strncpy(filename, c_str, PATH_MAX);
    (*env)->ReleaseStringUTFChars(env, j_str, c_str);

    (*env)->PopLocalFrame(env, NULL);
    return JNI_OK;
}

int match_ext_from_content(const char *content, const char *extensions) {
    char filename[PATH_MAX]; // = DocumentFile.fromSingleUri(appContext, contentUri).getName();
    if (get_filename_from_content(content, filename) != JNI_OK)
        return 0;
    LOGE("%s:%d recovered name '%s'", __PRETTY_FUNCTION__, __LINE__, filename);
    return av_match_ext(filename, extensions);
}
/**
 * Sets log level.
 *
 * @param env pointer to native method interface
 * @param object reference to the class on which this method is invoked
 * @param level log level
 */
JNIEXPORT void JNICALL Java_com_arthenica_mobileffmpeg_Config_setNativeLogLevel(JNIEnv *env, jclass object, jint level) {
    av_log_set_level(level);
}

/**
 * Returns current log level.
 *
 * @param env pointer to native method interface
 * @param object reference to the class on which this method is invoked
 */
JNIEXPORT jint JNICALL Java_com_arthenica_mobileffmpeg_Config_getNativeLogLevel(JNIEnv *env, jclass object) {
    return av_log_get_level();
}

/**
 * Enables log and statistics redirection.
 *
 * @param env pointer to native method interface
 * @param object reference to the class on which this method is invoked
 */
JNIEXPORT void JNICALL Java_com_arthenica_mobileffmpeg_Config_enableNativeRedirection(JNIEnv *env, jclass object) {
    mutexLock();

    if (redirectionEnabled != 0) {
        mutexUnlock();
        return;
    }
    redirectionEnabled = 1;

    mutexUnlock();

    int rc = pthread_create(&callbackThread, 0, callbackThreadFunction, 0);
    if (rc != 0) {
        LOGE("Failed to create callback thread (rc=%d).\n", rc);
        return;
    }

    av_log_set_callback(mobileffmpeg_log_callback_function);
    set_report_callback(mobileffmpeg_statistics_callback_function);
}

/**
 * Disables log and statistics redirection.
 *
 * @param env pointer to native method interface
 * @param object reference to the class on which this method is invoked
 */
JNIEXPORT void JNICALL Java_com_arthenica_mobileffmpeg_Config_disableNativeRedirection(JNIEnv *env, jclass object) {

    mutexLock();

    if (redirectionEnabled != 1) {
        mutexUnlock();
        return;
    }
    redirectionEnabled = 0;

    mutexUnlock();

    av_log_set_callback(av_log_default_callback);
    set_report_callback(NULL);

    monitorNotify();
}

/**
 * Returns FFmpeg version bundled within the library natively.
 *
 * @param env pointer to native method interface
 * @param object reference to the class on which this method is invoked
 * @return FFmpeg version string
 */
JNIEXPORT jstring JNICALL Java_com_arthenica_mobileffmpeg_Config_getNativeFFmpegVersion(JNIEnv *env, jclass object) {
    return (*env)->NewStringUTF(env, FFMPEG_VERSION);
}

/**
 * Returns MobileFFmpeg library version natively.
 *
 * @param env pointer to native method interface
 * @param object reference to the class on which this method is invoked
 * @return MobileFFmpeg version string
 */
JNIEXPORT jstring JNICALL Java_com_arthenica_mobileffmpeg_Config_getNativeVersion(JNIEnv *env, jclass object) {
    return (*env)->NewStringUTF(env, MOBILE_FFMPEG_VERSION);
}

/**
 * Synchronously executes FFmpeg natively with arguments provided.
 *
 * @param env pointer to native method interface
 * @param object reference to the class on which this method is invoked
 * @param stringArray reference to the object holding FFmpeg command arguments
 * @return zero on successful execution, non-zero on error
 */
JNIEXPORT jint JNICALL Java_com_arthenica_mobileffmpeg_Config_nativeFFmpegExecute(JNIEnv *env, jclass object, jobjectArray stringArray) {
    jstring *tempArray = NULL;
    int argumentCount = 1;
    char **argv = NULL;

    if (stringArray != NULL) {
        int programArgumentCount = (*env)->GetArrayLength(env, stringArray);
        argumentCount = programArgumentCount + 1;

        tempArray = (jstring *) av_malloc(sizeof(jstring) * programArgumentCount);
    }

    /* PRESERVE USAGE FORMAT
     *
     * ffmpeg <arguments>
     */
    argv = (char **)av_malloc(sizeof(char*) * (argumentCount));
    argv[0] = (char *)av_malloc(sizeof(char) * (strlen(LIB_NAME) + 1));
    strcpy(argv[0], LIB_NAME);

    // PREPARE
    if (stringArray != NULL) {
        for (int i = 0; i < (argumentCount - 1); i++) {
            tempArray[i] = (jstring) (*env)->GetObjectArrayElement(env, stringArray, i);
            if (tempArray[i] != NULL) {
                argv[i + 1] = (char *) (*env)->GetStringUTFChars(env, tempArray[i], 0);
            }
        }
    }

    // LAST COMMAND OUTPUT SHOULD BE CLEARED BEFORE STARTING A NEW EXECUTION
    clearLastCommandOutput();

    // RUN
    int retCode = ffmpeg_execute(argumentCount, argv);

    // CLEANUP
    if (tempArray != NULL) {
        for (int i = 0; i < (argumentCount - 1); i++) {
            (*env)->ReleaseStringUTFChars(env, tempArray[i], argv[i + 1]);
        }

        av_free(tempArray);
    }
    av_free(argv[0]);
    av_free(argv);

    return retCode;
}

/**
 * Cancels an ongoing FFmpeg operation natively.
 *
 * @param env pointer to native method interface
 * @param object reference to the class on which this method is invoked
 */
JNIEXPORT void JNICALL Java_com_arthenica_mobileffmpeg_Config_nativeFFmpegCancel(JNIEnv *env, jclass object) {
    cancel_operation();
}

/**
 * Creates natively a new named pipe to use in FFmpeg operations.
 *
 * @param env pointer to native method interface
 * @param object reference to the class on which this method is invoked
 * @param ffmpegPipePath full path of ffmpeg pipe
 * @return zero on successful creation, non-zero on error
 */
JNIEXPORT int JNICALL Java_com_arthenica_mobileffmpeg_Config_registerNewNativeFFmpegPipe(JNIEnv *env, jclass object, jstring ffmpegPipePath) {
    const char *ffmpegPipePathString = (*env)->GetStringUTFChars(env, ffmpegPipePath, 0);

    return mkfifo(ffmpegPipePathString, S_IRWXU | S_IRWXG | S_IROTH);
}

/**
 * Returns MobileFFmpeg library build date natively.
 *
 * @param env pointer to native method interface
 * @param object reference to the class on which this method is invoked
 * @return MobileFFmpeg library build date
 */
JNIEXPORT jstring JNICALL Java_com_arthenica_mobileffmpeg_Config_getNativeBuildDate(JNIEnv *env, jclass object) {
    char buildDate[10];
    sprintf(buildDate, "%d", MOBILE_FFMPEG_BUILD_DATE);
    return (*env)->NewStringUTF(env, buildDate);
}

/**
 * Sets an environment variable natively
 *
 * @param env pointer to native method interface
 * @param object reference to the class on which this method is invoked
 * @param variableName environment variable name
 * @param variableValue environment variable value
 * @return zero on success, non-zero on error
 */
JNIEXPORT int JNICALL Java_com_arthenica_mobileffmpeg_Config_setNativeEnvironmentVariable(JNIEnv *env, jclass object, jstring variableName, jstring variableValue) {
    const char *variableNameString = (*env)->GetStringUTFChars(env, variableName, 0);
    const char *variableValueString = (*env)->GetStringUTFChars(env, variableValue, 0);

    return setenv(variableNameString, variableValueString, 1);
}

/**
 * Returns log output of the last executed command natively.
 *
 * @param env pointer to native method interface
 * @param object reference to the class on which this method is invoked
 * @return output of the last executed command
 */
JNIEXPORT jstring JNICALL Java_com_arthenica_mobileffmpeg_Config_getNativeLastCommandOutput(JNIEnv *env, jclass object) {
    int size = lastCommandOutput.len;
    if (size > 0) {
        jbyteArray byteArray = (*env)->NewByteArray(env, size);
        (*env)->SetByteArrayRegion(env, byteArray, 0, size, lastCommandOutput.str);
        jstring charsetName = (*env)->NewStringUTF(env, "UTF-8");
        return (jstring) (*env)->NewObject(env, stringClass, stringConstructor, byteArray, charsetName);
    }

    return (*env)->NewStringUTF(env, "");
}