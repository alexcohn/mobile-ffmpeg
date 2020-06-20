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

#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libavcodec/jni.h>
#include <libavformat/avformat.h>

#include "mobileffmpeg.h"

static jclass android_net_Uri = NULL; /* global ref */

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
static jobject android_app_Application_global_instance = NULL;
static jobject android_content_ContentResolver_global_instance = NULL;

// LOG with source filename and line
#define LOGD_NL(fmt, ...) LOGD("%s:%d " fmt, strrchr(__FILE__, '/')+1, __LINE__, __VA_ARGS__)
#define LOGE_NL(fmt, ...) LOGE("%s:%d " fmt, strrchr(__FILE__, '/')+1, __LINE__, __VA_ARGS__)

#define JNI_CHECK_EXTRA(j_object, extra_cleanup, format, ...) { \
    if (!(j_object) || (*env)->ExceptionCheck(env)) { \
        (*env)->ExceptionClear(env); \
        LOGE_NL("cannot get (" #j_object ") from " format, __VA_ARGS__); \
        extra_cleanup; \
        (*env)->PopLocalFrame(env, NULL); \
        return JNI_ERR; \
    } \
}

#define JNI_CHECK(j_object, format, ...) JNI_CHECK_EXTRA(j_object, ;, format, __VA_ARGS__)

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

/**
 * Kudos to Harlan Chen, https://stackoverflow.com/a/46871051

 static jobject getGlobalContext(JNIEnv *env)
 {

     jclass activityThread = (*env)->FindClass(env,"android/app/ActivityThread");
     jmethodID currentActivityThread = (*env)->GetStaticMethodID(env,activityThread, "currentActivityThread", "()Landroid/app/ActivityThread;");
     jobject at = (*env)->CallStaticObjectMethod(env,activityThread, currentActivityThread);

     jmethodID getApplication = (*env)->GetMethodID(env,activityThread, "getApplication", "()Landroid/app/Application;");
     jobject context = (*env)->CallObjectMethod(env,at, getApplication);
     return context;
 }
 */
int get_global_app_context(JNIEnv *env) {

    if (android_app_Application_global_instance != NULL)
        return JNI_OK;

    jclass android_app_ActivityThread;
    jmethodID android_app_ActivityThread_currentActivityThread;

    if (get_static_method_id(env, "android/app/ActivityThread", "currentActivityThread", "()Landroid/app/ActivityThread;", &android_app_ActivityThread, &android_app_ActivityThread_currentActivityThread) != JNI_OK)
        return JNI_ERR;

    jobject activity_thread_instance = (*env)->CallStaticObjectMethod(env, android_app_ActivityThread, android_app_ActivityThread_currentActivityThread);
    JNI_CHECK(activity_thread_instance, "%p %s", android_app_ActivityThread, "android/app/ActivityThread");

    jmethodID android_app_ActivityThread_getApplication = (*env)->GetMethodID(env, android_app_ActivityThread, "getApplication", "()Landroid/app/Application;");
    JNI_CHECK(android_app_ActivityThread_getApplication, "%p class %s method %s for %s", android_app_ActivityThread, "android/app/ActivityThread", "getApplication", "()Landroid/app/Application;");

    jobject context = (*env)->CallObjectMethod(env, activity_thread_instance, android_app_ActivityThread_getApplication);
    JNI_CHECK(context, "%p", android_app_ActivityThread);

    android_app_Application_global_instance = (*env)->NewGlobalRef(env, context);
    JNI_CHECK(android_app_Application_global_instance, "NewGlobalRef %p", context);

    return JNI_OK;
}

/**
  * Kudos to Stefan Haustein, https://stackoverflow.com/a/25005243

  Cursor cursor = getContentResolver().query(uri, null, null, null, null);
    try {
      if (cursor != null && cursor.moveToFirst()) {
        result = cursor.getString(cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DISPLAY_NAME));
      }
    } finally {
      cursor.close();
    }

 * note: it's OK not to call cursor.close() for cleanup, because it is
**/

static int get_filename_from_content(const char *content, char filename[]) {

    if (!android_content_ContentResolver_global_instance) {
        LOGE_NL("have not initialized %s instance", "android.content.ContentResolver");
        return JNI_ERR;
    }

    JavaVM *java_vm = av_jni_get_java_vm(NULL);
    JNIEnv *env;
    if ((*java_vm)->GetEnv(java_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK || !env) {
        LOGE_NL("cannot get env for %p", java_vm);
        return JNI_ERR;
    }

    JNI_CHECK((*env)->PushLocalFrame(env, 10) == 0, "%p", env);

    jstring uriString = (*env)->NewStringUTF(env, content);
    JNI_CHECK(uriString, "%s", content);

    jobject uri = (*env)->CallStaticObjectMethod(env, android_net_Uri, android_net_Uri_parse, uriString);
    JNI_CHECK(uri, "%p", uriString);

    jobject cursor = (*env)->CallObjectMethod(env, android_content_ContentResolver_global_instance, android_content_ContentResolver_query, uri, NULL, NULL, NULL, NULL);
    JNI_CHECK(cursor, "%p (%s, null, null, null, null)", android_content_ContentResolver_global_instance, content);

    jboolean move_res = (*env)->CallBooleanMethod(env, cursor, android_database_Cursor_moveToFirst);
    JNI_CHECK_EXTRA(move_res, (*env)->CallVoidMethod(env, cursor, android_database_Cursor_close), "%p moveToFirst()", cursor);

    jint column_idx = (*env)->CallIntMethod(env, cursor, android_database_Cursor_getColumnIndex, DISPLAY_NAME);
    JNI_CHECK_EXTRA(column_idx >= 0, (*env)->CallVoidMethod(env, cursor, android_database_Cursor_close), "%p getColumnIndex(DISPLAY_NAME)", cursor);

    jstring j_str = (jstring)(*env)->CallObjectMethod(env, cursor, android_database_Cursor_getString, column_idx);
    JNI_CHECK_EXTRA(j_str, (*env)->CallVoidMethod(env, cursor, android_database_Cursor_close), "%p getString (%d)", cursor, column_idx);
    (*env)->CallVoidMethod(env, cursor, android_database_Cursor_close);

    const char *c_str = (*env)->GetStringUTFChars(env, j_str, 0);
    JNI_CHECK(c_str, "GetStringUTFChars %p", j_str);

    strncpy(filename, c_str, PATH_MAX);
    (*env)->ReleaseStringUTFChars(env, j_str, c_str);

    (*env)->PopLocalFrame(env, NULL);
    return JNI_OK;
}

int match_ext_from_content(const char *content, const char *extensions) {
// TODO: keep last content ptr and its filename cached (in TLS);
    char filename[PATH_MAX]; // = DocumentFile.fromSingleUri(appContext, contentUri).getName();
    if (get_filename_from_content(content, filename) != JNI_OK)
        return 0;
    LOGD("recovered name '%s' for '%s'", filename, extensions);
    return av_match_ext(filename, extensions);
}

int get_fd_from_content(const char *content, int access) {

    int fd = -1; // it's lucky that JNI_ERR == -1

    JavaVM *java_vm = av_jni_get_java_vm(NULL);
    JNIEnv *env;
    if ((*java_vm)->GetEnv(java_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK || !env) {
        LOGE_NL("cannot get env for %p", java_vm);
        return JNI_ERR;
    }

    JNI_CHECK((*env)->PushLocalFrame(env, 10) == 0, "%p", env);

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
        DISPLAY_NAME = (*env)->NewGlobalRef(env, DISPLAY_NAME);
        JNI_CHECK(DISPLAY_NAME, "DocumentsContract.Document.COLUMN_DISPLAY_NAME=%s", "_display_name");

        if (get_global_app_context(env) != JNI_OK)
            return JNI_ERR;

        jobject contentResolver = (*env)->CallObjectMethod(env, android_app_Application_global_instance, android_content_Context_getContentResolver);
        JNI_CHECK(contentResolver, "getContentResolver from %p", android_app_Application_global_instance);

        android_content_ContentResolver_global_instance = (*env)->NewGlobalRef(env, contentResolver);
        JNI_CHECK(android_content_ContentResolver_global_instance, "NewGlobalRef %p", contentResolver);
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

    jobject parcelFileDescriptor = (*env)->CallObjectMethod(env, android_content_ContentResolver_global_instance, android_content_ContentResolver_openFileDescriptor, uri, fmodeString);
    JNI_CHECK(parcelFileDescriptor, "%p (%s, %s)", android_content_ContentResolver_global_instance, content, fmode);

    fd = (*env)->CallIntMethod(env, parcelFileDescriptor, android_os_ParcelFileDescriptor_getFd);
    JNI_CHECK(fd >= 0, "%p", parcelFileDescriptor);

    (*env)->PopLocalFrame(env, NULL);
    return dup(fd);
}
