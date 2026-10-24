/*
 * Minimal JNI definition header for Android Zygisk host compilation and testing.
 * Compatible with Android NDK and Standard JNI specifications.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t  jboolean;
typedef int8_t   jbyte;
typedef uint16_t jchar;
typedef int16_t  jshort;
typedef int32_t  jint;
typedef int64_t  jlong;
typedef float    jfloat;
typedef double   jdouble;
typedef jint     jsize;

#ifdef __cplusplus
class _jobject {};
class _jclass : public _jobject {};
class _jthrowable : public _jobject {};
class _jstring : public _jobject {};
class _jarray : public _jobject {};
class _jbooleanArray : public _jarray {};
class _jbyteArray : public _jarray {};
class _jcharArray : public _jarray {};
class _jshortArray : public _jarray {};
class _jintArray : public _jarray {};
class _jlongArray : public _jarray {};
class _jfloatArray : public _jarray {};
class _jdoubleArray : public _jarray {};
class _jobjectArray : public _jarray {};

typedef _jobject*       jobject;
typedef _jclass*        jclass;
typedef _jthrowable*    jthrowable;
typedef _jstring*       jstring;
typedef _jarray*        jarray;
typedef _jbooleanArray* jbooleanArray;
typedef _jbyteArray*    jbyteArray;
typedef _jcharArray*    jcharArray;
typedef _jshortArray*   jshortArray;
typedef _jintArray*     jintArray;
typedef _jlongArray*    jlongArray;
typedef _jfloatArray*   jfloatArray;
typedef _jdoubleArray*  jdoubleArray;
typedef _jobjectArray*  jobjectArray;
#else
typedef void* jobject;
typedef void* jclass;
typedef void* jthrowable;
typedef void* jstring;
typedef void* jarray;
typedef void* jbooleanArray;
typedef void* jbyteArray;
typedef void* jcharArray;
typedef void* jshortArray;
typedef void* jintArray;
typedef void* jlongArray;
typedef void* jfloatArray;
typedef void* jdoubleArray;
typedef void* jobjectArray;
#endif

struct _jfieldID;
typedef struct _jfieldID* jfieldID;

struct _jmethodID;
typedef struct _jmethodID* jmethodID;

typedef struct {
    const char* name;
    const char* signature;
    void*       fnPtr;
} JNINativeMethod;

struct JNINativeInterface;

#ifdef __cplusplus
struct JNIEnv_ {
    const struct JNINativeInterface* functions;

    jclass FindClass(const char* name) {
        return nullptr;
    }
    jfieldID GetStaticFieldID(jclass clazz, const char* name, const char* sig) {
        return nullptr;
    }
    jfieldID GetFieldID(jclass clazz, const char* name, const char* sig) {
        return nullptr;
    }
    jobject GetStaticObjectField(jclass clazz, jfieldID fieldID) {
        return nullptr;
    }
    void SetStaticObjectField(jclass clazz, jfieldID fieldID, jobject val) {}
    void SetStaticIntField(jclass clazz, jfieldID fieldID, jint val) {}
    jobject GetObjectField(jobject obj, jfieldID fieldID) {
        return nullptr;
    }
    jclass GetObjectClass(jobject obj) {
        return nullptr;
    }

    jmethodID GetMethodID(jclass clazz, const char* name, const char* sig) {
        return nullptr;
    }
    jmethodID GetStaticMethodID(jclass clazz, const char* name, const char* sig) {
        return nullptr;
    }

    jobject NewObject(jclass clazz, jmethodID methodID, ...) {
        return nullptr;
    }
    jobject NewDirectByteBuffer(void* address, jlong capacity) {
        return nullptr;
    }
    jobjectArray NewObjectArray(jsize len, jclass clazz, jobject init) {
        return nullptr;
    }

    jobject CallObjectMethod(jobject obj, jmethodID methodID, ...) {
        return nullptr;
    }
    void CallVoidMethod(jobject obj, jmethodID methodID, ...) {}
    jobject CallStaticObjectMethod(jclass clazz, jmethodID methodID, ...) {
        return nullptr;
    }
    void CallStaticVoidMethod(jclass clazz, jmethodID methodID, ...) {}

    const char* GetStringUTFChars(jstring str, jboolean* isCopy) {
        return "";
    }
    void ReleaseStringUTFChars(jstring str, const char* chars) {}
    jstring NewStringUTF(const char* bytes) {
        return nullptr;
    }

    jobject NewGlobalRef(jobject lobj) {
        return lobj;
    }
    void DeleteGlobalRef(jobject gref) {}
    void DeleteLocalRef(jobject obj) {}

    void ExceptionClear() {}
    jboolean ExceptionCheck() { return 0; }
};
typedef JNIEnv_ JNIEnv;
#else
typedef const struct JNINativeInterface* JNIEnv;
#endif

#ifdef __cplusplus
}
#endif
