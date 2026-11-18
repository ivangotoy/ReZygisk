/* Host stand-in for the NDK's android/log.h: logging is dropped. */
#ifndef TESTS_ANDROID_LOG_H
#define TESTS_ANDROID_LOG_H

enum {
  ANDROID_LOG_VERBOSE = 2,
  ANDROID_LOG_DEBUG,
  ANDROID_LOG_INFO,
  ANDROID_LOG_WARN,
  ANDROID_LOG_ERROR,
  ANDROID_LOG_FATAL
};

static inline int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio; (void)tag; (void)fmt;
  return 0;
}

#endif
