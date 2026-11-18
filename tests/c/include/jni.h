/* Host stand-in for the NDK's jni.h, for tests only: the JNI types and
   the JNIEnv/JavaVM members that loader/src/injector uses. Tests fill
   the function tables with their own fakes; nothing here talks to a VM. */
#ifndef TESTS_JNI_H
#define TESTS_JNI_H

#include <stdarg.h>
#include <stdint.h>

typedef uint8_t jboolean;
typedef int8_t jbyte;
typedef uint16_t jchar;
typedef int16_t jshort;
typedef int32_t jint;
typedef int64_t jlong;
typedef float jfloat;
typedef double jdouble;
typedef jint jsize;

typedef void *jobject;
typedef jobject jclass;
typedef jobject jstring;
typedef jobject jarray;
typedef jarray jobjectArray;
typedef jarray jintArray;
typedef jarray jlongArray;
typedef jarray jbooleanArray;
typedef jarray jbyteArray;
typedef jobject jthrowable;
typedef struct _jfieldID *jfieldID;
typedef struct _jmethodID *jmethodID;

typedef struct {
  const char *name;
  const char *signature;
  void *fnPtr;
} JNINativeMethod;

#define JNI_FALSE 0
#define JNI_TRUE 1
#define JNI_OK 0
#define JNI_ERR (-1)
#define JNI_ABORT 2
#define JNI_VERSION_1_6 0x00010006

struct JNINativeInterface;
struct JNIInvokeInterface;
typedef const struct JNINativeInterface *JNIEnv;
typedef const struct JNIInvokeInterface *JavaVM;

struct JNINativeInterface {
  jclass (*FindClass)(JNIEnv *, const char *);
  jmethodID (*FromReflectedMethod)(JNIEnv *, jobject);
  jobject (*ToReflectedMethod)(JNIEnv *, jclass, jmethodID, jboolean);
  jboolean (*ExceptionCheck)(JNIEnv *);
  void (*ExceptionClear)(JNIEnv *);
  void (*DeleteLocalRef)(JNIEnv *, jobject);
  jmethodID (*GetMethodID)(JNIEnv *, jclass, const char *, const char *);
  jmethodID (*GetStaticMethodID)(JNIEnv *, jclass, const char *, const char *);
  jobject (*CallObjectMethod)(JNIEnv *, jobject, jmethodID, ...);
  jint (*CallIntMethod)(JNIEnv *, jobject, jmethodID, ...);
  jfieldID (*GetFieldID)(JNIEnv *, jclass, const char *, const char *);
  jlong (*GetLongField)(JNIEnv *, jobject, jfieldID);
  jfieldID (*GetStaticFieldID)(JNIEnv *, jclass, const char *, const char *);
  jint (*GetStaticIntField)(JNIEnv *, jclass, jfieldID);
  jsize (*GetArrayLength)(JNIEnv *, jarray);
  jobject (*GetObjectArrayElement)(JNIEnv *, jobjectArray, jsize);
  jint *(*GetIntArrayElements)(JNIEnv *, jintArray, jboolean *);
  void (*ReleaseIntArrayElements)(JNIEnv *, jintArray, jint *, jint);
  jint (*RegisterNatives)(JNIEnv *, jclass, const JNINativeMethod *, jint);
  const char *(*GetStringUTFChars)(JNIEnv *, jstring, jboolean *);
  void (*ReleaseStringUTFChars)(JNIEnv *, jstring, const char *);
  jintArray (*NewIntArray)(JNIEnv *, jsize);
  void (*SetIntArrayRegion)(JNIEnv *, jintArray, jsize, jsize, const jint *);
};

struct JNIInvokeInterface {
  jint (*GetEnv)(JavaVM *, void **, jint);
};

#endif
