// tvpgl_route.h - the SIMD dispatch table the upstream build generates
//
// visual/tvpgl.cpp declares every blitter as a function pointer and assigns
// the generic C implementation to each of them in TVPInitTVPGL(); the routing
// header the Android build generates only picks faster implementations on top.
// A desktop build without that generator uses the C path as it stands, so this
// header has nothing to add: it exists so the include resolves, and the comment
// records why it is empty.
#ifndef KRKR_COMPAT_TVPGL_ROUTE_H
#define KRKR_COMPAT_TVPGL_ROUTE_H

#endif // KRKR_COMPAT_TVPGL_ROUTE_H
