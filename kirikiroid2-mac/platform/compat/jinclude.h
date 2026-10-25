/*
 * jinclude.h - not shipped by libjpeg-turbo
 *
 * The engine's LoadJPEG.cpp includes libjpeg's internal jinclude.h before
 * jpeglib.h, as its copy of libjpeg carried it.  libjpeg-turbo's public headers
 * do not, and nothing the file uses comes from it: the declarations it needs are
 * in jpeglib.h and jerror.h, which are included right after.  So this is empty
 * on purpose.
 */
#ifndef KRKR_COMPAT_JINCLUDE_H
#define KRKR_COMPAT_JINCLUDE_H
#endif
