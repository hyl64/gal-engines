// Protect.h - the win32 anti-tamper subsystem, absent on the desktop build
//
// The Windows build starts a protection layer before the engine boots
// (base/SysInitIntf.cpp calls TVPProtectInit).  A desktop build has no such
// subsystem and nothing to protect; booting straight away is what the call
// means here.
#ifndef KRKR_COMPAT_PROTECT_H
#define KRKR_COMPAT_PROTECT_H

static inline bool TVPProtectInit()
{
	return true;
}

static inline void TVPProtectExit()
{
}

#endif // KRKR_COMPAT_PROTECT_H
