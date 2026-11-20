// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project
#![allow(non_camel_case_types, non_snake_case, dead_code)]

use crate::zygisk_abi::JNINativeMethod;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};

pub type jboolean = u8;
pub type jbyte = i8;
pub type jchar = u16;
pub type jshort = i16;
pub type jint = i32;
pub type jlong = i64;
pub type jfloat = f32;
pub type jdouble = f64;
pub type jsize = jint;

pub type jobject = *mut c_void;
pub type jclass = jobject;
pub type jstring = jobject;
pub type jarray = jobject;
pub type jobjectArray = jarray;
pub type jintArray = jarray;
pub type jfieldID = *mut c_void;
pub type jmethodID = *mut c_void;

pub type JNIEnv = *const JNINativeInterface;

/// Mirrors the standard Android NDK `JNINativeInterface_` function pointer table.
#[repr(C)]
pub struct JNINativeInterface {
    pub reserved0: *mut c_void,
    pub reserved1: *mut c_void,
    pub reserved2: *mut c_void,
    pub reserved3: *mut c_void,
    pub GetVersion: unsafe extern "C" fn(*mut JNIEnv) -> jint,
    pub DefineClass: *mut c_void,
    pub FindClass: unsafe extern "C" fn(*mut JNIEnv, *const c_char) -> jclass,
    pub FromReflectedMethod: *mut c_void,
    pub FromReflectedField: *mut c_void,
    pub ToReflectedMethod: *mut c_void,
    pub GetSuperclass: *mut c_void,
    pub IsAssignableFrom: *mut c_void,
    pub ToReflectedField: *mut c_void,
    pub Throw: *mut c_void,
    pub ThrowNew: *mut c_void,
    pub ExceptionOccurred: *mut c_void,
    pub ExceptionDescribe: *mut c_void,
    pub ExceptionClear: unsafe extern "C" fn(*mut JNIEnv),
    pub FatalError: *mut c_void,
    pub PushLocalFrame: *mut c_void,
    pub PopLocalFrame: *mut c_void,
    pub NewGlobalRef: unsafe extern "C" fn(*mut JNIEnv, jobject) -> jobject,
    pub DeleteGlobalRef: unsafe extern "C" fn(*mut JNIEnv, jobject),
    pub DeleteLocalRef: unsafe extern "C" fn(*mut JNIEnv, jobject),
    pub IsSameObject: *mut c_void,
    pub NewLocalRef: *mut c_void,
    pub EnsureLocalCapacity: *mut c_void,
    pub AllocObject: *mut c_void,
    pub NewObject: unsafe extern "C" fn(*mut JNIEnv, jclass, jmethodID, ...) -> jobject,
    pub NewObjectV: *mut c_void,
    pub NewObjectA: *mut c_void,
    pub GetObjectClass: unsafe extern "C" fn(*mut JNIEnv, jobject) -> jclass,
    pub IsInstanceOf: *mut c_void,
    pub GetMethodID:
        unsafe extern "C" fn(*mut JNIEnv, jclass, *const c_char, *const c_char) -> jmethodID,
    pub CallObjectMethod:
        unsafe extern "C" fn(*mut JNIEnv, jobject, jmethodID, ...) -> jobject,
    pub CallObjectMethodV: *mut c_void,
    pub CallObjectMethodA: *mut c_void,
    pub CallBooleanMethod: *mut c_void,
    pub CallBooleanMethodV: *mut c_void,
    pub CallBooleanMethodA: *mut c_void,
    pub CallByteMethod: *mut c_void,
    pub CallByteMethodV: *mut c_void,
    pub CallByteMethodA: *mut c_void,
    pub CallCharMethod: *mut c_void,
    pub CallCharMethodV: *mut c_void,
    pub CallCharMethodA: *mut c_void,
    pub CallShortMethod: *mut c_void,
    pub CallShortMethodV: *mut c_void,
    pub CallShortMethodA: *mut c_void,
    pub CallIntMethod: *mut c_void,
    pub CallIntMethodV: *mut c_void,
    pub CallIntMethodA: *mut c_void,
    pub CallLongMethod: *mut c_void,
    pub CallLongMethodV: *mut c_void,
    pub CallLongMethodA: *mut c_void,
    pub CallFloatMethod: *mut c_void,
    pub CallFloatMethodV: *mut c_void,
    pub CallFloatMethodA: *mut c_void,
    pub CallDoubleMethod: *mut c_void,
    pub CallDoubleMethodV: *mut c_void,
    pub CallDoubleMethodA: *mut c_void,
    pub CallVoidMethod: unsafe extern "C" fn(*mut JNIEnv, jobject, jmethodID, ...),
    pub CallVoidMethodV: *mut c_void,
    pub CallVoidMethodA: *mut c_void,
    pub CallNonvirtualObjectMethod: *mut c_void,
    pub CallNonvirtualObjectMethodV: *mut c_void,
    pub CallNonvirtualObjectMethodA: *mut c_void,
    pub CallNonvirtualBooleanMethod: *mut c_void,
    pub CallNonvirtualBooleanMethodV: *mut c_void,
    pub CallNonvirtualBooleanMethodA: *mut c_void,
    pub CallNonvirtualByteMethod: *mut c_void,
    pub CallNonvirtualByteMethodV: *mut c_void,
    pub CallNonvirtualByteMethodA: *mut c_void,
    pub CallNonvirtualCharMethod: *mut c_void,
    pub CallNonvirtualCharMethodV: *mut c_void,
    pub CallNonvirtualCharMethodA: *mut c_void,
    pub CallNonvirtualShortMethod: *mut c_void,
    pub CallNonvirtualShortMethodV: *mut c_void,
    pub CallNonvirtualShortMethodA: *mut c_void,
    pub CallNonvirtualIntMethod: *mut c_void,
    pub CallNonvirtualIntMethodV: *mut c_void,
    pub CallNonvirtualIntMethodA: *mut c_void,
    pub CallNonvirtualLongMethod: *mut c_void,
    pub CallNonvirtualLongMethodV: *mut c_void,
    pub CallNonvirtualLongMethodA: *mut c_void,
    pub CallNonvirtualFloatMethod: *mut c_void,
    pub CallNonvirtualFloatMethodV: *mut c_void,
    pub CallNonvirtualFloatMethodA: *mut c_void,
    pub CallNonvirtualDoubleMethod: *mut c_void,
    pub CallNonvirtualDoubleMethodV: *mut c_void,
    pub CallNonvirtualDoubleMethodA: *mut c_void,
    pub CallNonvirtualVoidMethod: *mut c_void,
    pub CallNonvirtualVoidMethodV: *mut c_void,
    pub CallNonvirtualVoidMethodA: *mut c_void,
    pub GetFieldID:
        unsafe extern "C" fn(*mut JNIEnv, jclass, *const c_char, *const c_char) -> jfieldID,
    pub GetObjectField: unsafe extern "C" fn(*mut JNIEnv, jobject, jfieldID) -> jobject,
    pub GetBooleanField: *mut c_void,
    pub GetByteField: *mut c_void,
    pub GetCharField: *mut c_void,
    pub GetShortField: *mut c_void,
    pub GetIntField: *mut c_void,
    pub GetLongField: *mut c_void,
    pub GetFloatField: *mut c_void,
    pub GetDoubleField: *mut c_void,
    pub SetObjectField: *mut c_void,
    pub SetBooleanField: *mut c_void,
    pub SetByteField: *mut c_void,
    pub SetCharField: *mut c_void,
    pub SetShortField: *mut c_void,
    pub SetIntField: *mut c_void,
    pub SetLongField: *mut c_void,
    pub SetFloatField: *mut c_void,
    pub SetDoubleField: *mut c_void,
    pub GetStaticMethodID:
        unsafe extern "C" fn(*mut JNIEnv, jclass, *const c_char, *const c_char) -> jmethodID,
    pub CallStaticObjectMethod:
        unsafe extern "C" fn(*mut JNIEnv, jclass, jmethodID, ...) -> jobject,
    pub CallStaticObjectMethodV: *mut c_void,
    pub CallStaticObjectMethodA: *mut c_void,
    pub CallStaticBooleanMethod:
        unsafe extern "C" fn(*mut JNIEnv, jclass, jmethodID, ...) -> jboolean,
    pub CallStaticBooleanMethodV: *mut c_void,
    pub CallStaticBooleanMethodA: *mut c_void,
    pub CallStaticByteMethod: *mut c_void,
    pub CallStaticByteMethodV: *mut c_void,
    pub CallStaticByteMethodA: *mut c_void,
    pub CallStaticCharMethod: *mut c_void,
    pub CallStaticCharMethodV: *mut c_void,
    pub CallStaticCharMethodA: *mut c_void,
    pub CallStaticShortMethod: *mut c_void,
    pub CallStaticShortMethodV: *mut c_void,
    pub CallStaticShortMethodA: *mut c_void,
    pub CallStaticIntMethod: unsafe extern "C" fn(*mut JNIEnv, jclass, jmethodID, ...) -> jint,
    pub CallStaticIntMethodV: *mut c_void,
    pub CallStaticIntMethodA: *mut c_void,
    pub CallStaticLongMethod: *mut c_void,
    pub CallStaticLongMethodV: *mut c_void,
    pub CallStaticLongMethodA: *mut c_void,
    pub CallStaticFloatMethod: *mut c_void,
    pub CallStaticFloatMethodV: *mut c_void,
    pub CallStaticFloatMethodA: *mut c_void,
    pub CallStaticDoubleMethod: *mut c_void,
    pub CallStaticDoubleMethodV: *mut c_void,
    pub CallStaticDoubleMethodA: *mut c_void,
    pub CallStaticVoidMethod: unsafe extern "C" fn(*mut JNIEnv, jclass, jmethodID, ...),
    pub CallStaticVoidMethodV: *mut c_void,
    pub CallStaticVoidMethodA: *mut c_void,
    pub GetStaticFieldID:
        unsafe extern "C" fn(*mut JNIEnv, jclass, *const c_char, *const c_char) -> jfieldID,
    pub GetStaticObjectField: unsafe extern "C" fn(*mut JNIEnv, jclass, jfieldID) -> jobject,
    pub GetStaticBooleanField: *mut c_void,
    pub GetStaticByteField: *mut c_void,
    pub GetStaticCharField: *mut c_void,
    pub GetStaticShortField: *mut c_void,
    pub GetStaticIntField: *mut c_void,
    pub GetStaticLongField: *mut c_void,
    pub GetStaticFloatField: *mut c_void,
    pub GetStaticDoubleField: *mut c_void,
    pub SetStaticObjectField: unsafe extern "C" fn(*mut JNIEnv, jclass, jfieldID, jobject),
    pub SetStaticBooleanField: *mut c_void,
    pub SetStaticByteField: *mut c_void,
    pub SetStaticCharField: *mut c_void,
    pub SetStaticShortField: *mut c_void,
    pub SetStaticIntField: unsafe extern "C" fn(*mut JNIEnv, jclass, jfieldID, jint),
    pub SetStaticLongField: *mut c_void,
    pub SetStaticFloatField: *mut c_void,
    pub SetStaticDoubleField: *mut c_void,
    pub NewString: *mut c_void,
    pub GetStringLength: *mut c_void,
    pub GetStringChars: *mut c_void,
    pub ReleaseStringChars: *mut c_void,
    pub NewStringUTF: unsafe extern "C" fn(*mut JNIEnv, *const c_char) -> jstring,
    pub GetStringUTFLength: *mut c_void,
    pub GetStringUTFChars:
        unsafe extern "C" fn(*mut JNIEnv, jstring, *mut jboolean) -> *const c_char,
    pub ReleaseStringUTFChars: unsafe extern "C" fn(*mut JNIEnv, jstring, *const c_char),
    pub GetArrayLength: *mut c_void,
    pub NewObjectArray: unsafe extern "C" fn(*mut JNIEnv, jsize, jclass, jobject) -> jobjectArray,
    pub GetObjectArrayElement: *mut c_void,
    pub SetObjectArrayElement: *mut c_void,
    pub NewBooleanArray: *mut c_void,
    pub NewByteArray: *mut c_void,
    pub NewCharArray: *mut c_void,
    pub NewShortArray: *mut c_void,
    pub NewIntArray: *mut c_void,
    pub NewLongArray: *mut c_void,
    pub NewFloatArray: *mut c_void,
    pub NewDoubleArray: *mut c_void,
    pub GetBooleanArrayElements: *mut c_void,
    pub GetByteArrayElements: *mut c_void,
    pub GetCharArrayElements: *mut c_void,
    pub GetShortArrayElements: *mut c_void,
    pub GetIntArrayElements: *mut c_void,
    pub GetLongArrayElements: *mut c_void,
    pub GetFloatArrayElements: *mut c_void,
    pub GetDoubleArrayElements: *mut c_void,
    pub ReleaseBooleanArrayElements: *mut c_void,
    pub ReleaseByteArrayElements: *mut c_void,
    pub ReleaseCharArrayElements: *mut c_void,
    pub ReleaseShortArrayElements: *mut c_void,
    pub ReleaseIntArrayElements: *mut c_void,
    pub ReleaseLongArrayElements: *mut c_void,
    pub ReleaseFloatArrayElements: *mut c_void,
    pub ReleaseDoubleArrayElements: *mut c_void,
    pub GetBooleanArrayRegion: *mut c_void,
    pub GetByteArrayRegion: *mut c_void,
    pub GetCharArrayRegion: *mut c_void,
    pub GetShortArrayRegion: *mut c_void,
    pub GetIntArrayRegion: *mut c_void,
    pub GetLongArrayRegion: *mut c_void,
    pub GetFloatArrayRegion: *mut c_void,
    pub GetDoubleArrayRegion: *mut c_void,
    pub SetBooleanArrayRegion: *mut c_void,
    pub SetByteArrayRegion: *mut c_void,
    pub SetCharArrayRegion: *mut c_void,
    pub SetShortArrayRegion: *mut c_void,
    pub SetIntArrayRegion: *mut c_void,
    pub SetLongArrayRegion: *mut c_void,
    pub SetFloatArrayRegion: *mut c_void,
    pub SetDoubleArrayRegion: *mut c_void,
    pub RegisterNatives:
        unsafe extern "C" fn(*mut JNIEnv, jclass, *const JNINativeMethod, jint) -> jint,
    pub UnregisterNatives: *mut c_void,
    pub MonitorEnter: *mut c_void,
    pub MonitorExit: *mut c_void,
    pub GetJavaVM: *mut c_void,
    pub GetStringRegion: *mut c_void,
    pub GetStringUTFRegion: *mut c_void,
    pub GetPrimitiveArrayCritical: *mut c_void,
    pub ReleasePrimitiveArrayCritical: *mut c_void,
    pub GetStringCritical: *mut c_void,
    pub ReleaseStringCritical: *mut c_void,
    pub NewWeakGlobalRef: *mut c_void,
    pub DeleteWeakGlobalRef: *mut c_void,
    pub ExceptionCheck: unsafe extern "C" fn(*mut JNIEnv) -> jboolean,
    pub NewDirectByteBuffer: unsafe extern "C" fn(*mut JNIEnv, *mut c_void, jlong) -> jobject,
    pub GetDirectBufferAddress: *mut c_void,
    pub GetDirectBufferCapacity: *mut c_void,
    pub GetObjectRefType: *mut c_void,
}

#[inline]
pub unsafe fn jni_vtbl<'a>(env: *mut JNIEnv) -> &'a JNINativeInterface {
    &*(*env)
}

pub unsafe fn exception_check_and_clear(env: *mut JNIEnv) -> bool {
    if env.is_null() {
        return false;
    }
    if (jni_vtbl(env).ExceptionCheck)(env) != 0 {
        (jni_vtbl(env).ExceptionClear)(env);
        true
    } else {
        false
    }
}

pub unsafe fn get_string_utf(env: *mut JNIEnv, jstr: jstring) -> String {
    if env.is_null() || jstr.is_null() {
        return String::new();
    }
    let chars = (jni_vtbl(env).GetStringUTFChars)(env, jstr, std::ptr::null_mut());
    if chars.is_null() {
        exception_check_and_clear(env);
        return String::new();
    }
    let s = CStr::from_ptr(chars).to_string_lossy().into_owned();
    (jni_vtbl(env).ReleaseStringUTFChars)(env, jstr, chars);
    s
}

pub unsafe fn new_string_utf(env: *mut JNIEnv, s: &str) -> jstring {
    if env.is_null() {
        return std::ptr::null_mut();
    }
    let Ok(c) = CString::new(s) else {
        return std::ptr::null_mut();
    };
    (jni_vtbl(env).NewStringUTF)(env, c.as_ptr())
}

pub unsafe fn find_class(env: *mut JNIEnv, name: &str) -> jclass {
    let Ok(c) = CString::new(name) else {
        return std::ptr::null_mut();
    };
    (jni_vtbl(env).FindClass)(env, c.as_ptr())
}

pub unsafe fn get_static_method_id(
    env: *mut JNIEnv,
    cls: jclass,
    name: &str,
    sig: &str,
) -> jmethodID {
    let (Ok(cn), Ok(cs)) = (CString::new(name), CString::new(sig)) else {
        return std::ptr::null_mut();
    };
    (jni_vtbl(env).GetStaticMethodID)(env, cls, cn.as_ptr(), cs.as_ptr())
}

pub unsafe fn get_method_id(env: *mut JNIEnv, cls: jclass, name: &str, sig: &str) -> jmethodID {
    let (Ok(cn), Ok(cs)) = (CString::new(name), CString::new(sig)) else {
        return std::ptr::null_mut();
    };
    (jni_vtbl(env).GetMethodID)(env, cls, cn.as_ptr(), cs.as_ptr())
}

pub unsafe fn get_static_field_id(
    env: *mut JNIEnv,
    cls: jclass,
    name: &str,
    sig: &str,
) -> jfieldID {
    let (Ok(cn), Ok(cs)) = (CString::new(name), CString::new(sig)) else {
        return std::ptr::null_mut();
    };
    (jni_vtbl(env).GetStaticFieldID)(env, cls, cn.as_ptr(), cs.as_ptr())
}

pub unsafe fn get_field_id(env: *mut JNIEnv, cls: jclass, name: &str, sig: &str) -> jfieldID {
    let (Ok(cn), Ok(cs)) = (CString::new(name), CString::new(sig)) else {
        return std::ptr::null_mut();
    };
    (jni_vtbl(env).GetFieldID)(env, cls, cn.as_ptr(), cs.as_ptr())
}

pub unsafe fn delete_local_ref(env: *mut JNIEnv, obj: jobject) {
    if !env.is_null() && !obj.is_null() {
        (jni_vtbl(env).DeleteLocalRef)(env, obj);
    }
}

pub unsafe fn set_static_string_field(env: *mut JNIEnv, cls: jclass, name: &str, val: &str) -> bool {
    if env.is_null() || cls.is_null() || val.is_empty() {
        return false;
    }
    let fid = get_static_field_id(env, cls, name, "Ljava/lang/String;");
    if exception_check_and_clear(env) || fid.is_null() {
        return false;
    }
    let jval = new_string_utf(env, val);
    if jval.is_null() {
        exception_check_and_clear(env);
        return false;
    }
    (jni_vtbl(env).SetStaticObjectField)(env, cls, fid, jval);
    let threw = exception_check_and_clear(env);
    delete_local_ref(env, jval);
    !threw
}

pub unsafe fn set_static_int_field(env: *mut JNIEnv, cls: jclass, name: &str, val: i32) -> bool {
    if env.is_null() || cls.is_null() || val <= 0 {
        return false;
    }
    let fid = get_static_field_id(env, cls, name, "I");
    if exception_check_and_clear(env) || fid.is_null() {
        return false;
    }
    (jni_vtbl(env).SetStaticIntField)(env, cls, fid, val);
    !exception_check_and_clear(env)
}
