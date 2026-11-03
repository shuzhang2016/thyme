#pragma once

// TSan annotation interface for the remaining seqlock payload reads that are
// provably race-free (a lease-protected region, or a documented benign race
// not yet converted to atomic byte access). Never apply it as a blanket
// ignore: a genuine concurrent access that this hides is a real bug.
//
// GCC's libsanitizer exports these symbols but ships no declaring header, so
// declare them ourselves; Clang's compiler-rt declares them identically.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define THYME_TSAN_ACTIVE 1
#endif
#elif defined(__SANITIZE_THREAD__)
#define THYME_TSAN_ACTIVE 1
#endif

#if defined(THYME_TSAN_ACTIVE)
extern "C" {
void AnnotateIgnoreReadsBegin(const char* file, int line);
void AnnotateIgnoreReadsEnd();
}
#endif
