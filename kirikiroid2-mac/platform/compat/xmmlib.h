// xmmlib.h - the vendored SIMD math library, absent from the desktop build
//
// sound/MathAlgorithms.h includes this header but uses no symbol from it: the
// helpers it declares are picked up only by the SSE paths of the DSP, and the
// desktop build compiles the portable fallbacks instead.  The include has to
// resolve, so this file exists and is empty.
#ifndef KRKR_COMPAT_XMMLIB_H
#define KRKR_COMPAT_XMMLIB_H

#endif // KRKR_COMPAT_XMMLIB_H
