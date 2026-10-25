/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * stubs.c - the macOS (SDL2) back end's not-yet-implemented surfaces
 *
 * The child windows, the text entry, the edit field, the script's
 * dialogs, the file and folder dialogs and the debugger's window are
 * real and live in dchild.c, dedit.c, duifield.c, duikit.c,
 * ddialogs.c and ddbgwin.c.  What is left here is the launcher's
 * "Select game" dialog (the engine's launcher mode), which the back end
 * does not provide yet: it fails gracefully, exactly like a machine
 * without the display.  OS_MessageBox is real: errors deserve a native
 * dialog, via SDL's own (UTF-8, so the Shift-JIS text converts), and an
 * unattended run (BGI_AUTO_DIALOG, BGI_MSGBOX_STDERR) gets the default
 * button without a dialog.
 */

#include "dui_internal.h"

// ---- the launcher's dialog ---------------------------------------------------------
// The launcher mode ("no game directory") needs a game chooser; this back
// end starts games directly, so the dialog reports "Quit".

int OS_LauncherDialog(OsLauncher_t* l)
{
	(void)l;
	return 0;
}

// ---- the message box -----------------------------------------------------------------
// the default button of the box, as MessageBox would choose it
static int MessageBoxDefault(uint32_t flags)
{
	switch(flags & 0x0f)
	{
	case OS_MB_OKCANCEL: return (flags & OS_MB_DEFBUTTON2) ? OS_IDCANCEL : OS_IDOK;
	case OS_MB_YESNO: return (flags & OS_MB_DEFBUTTON2) ? OS_IDNO : OS_IDYES;
	case OS_MB_RETRYCANCEL: return (flags & OS_MB_DEFBUTTON2) ? OS_IDCANCEL : OS_IDRETRY;
	default: return OS_IDOK;
	}
}

/* MessageBox over the main window: modal, the id of the button pressed
 * (the close box counts as cancel).  SDL's own dialog shows it natively;
 * the engine's text is Shift-JIS, so it converts to UTF-8 first.  A
 * BGI_MSGBOX_STDERR run (the tests) only prints it, and BGI_AUTO_DIALOG
 * presses the default button on its own so that an unattended boot is not
 * blocked by a question. */
int OS_MessageBox(const char* text, const char* caption, uint32_t flags)
{
	char utfText[0x4000], utfCap[0x400];
	SDL_MessageBoxData data;
	SDL_MessageBoxButtonData buttons[2];
	int type = SDL_MESSAGEBOX_INFORMATION;
	int nbuttons = 1;
	int ret;
	OsPosix_SjisToUtf8(text != NULL ? text : "", utfText, sizeof utfText);
	OsPosix_SjisToUtf8(caption != NULL ? caption : "", utfCap, sizeof utfCap);
	// BGI_MSGBOX_STDERR: an unattended run (the tests) wants the text on
	// stderr and the default button, never a dialog that waits for a click
	if(getenv("BGI_MSGBOX_STDERR") != NULL || Dui_AutoChoice() != DUI_AUTO_NONE)
	{
		ret = MessageBoxDefault(flags);
		fprintf(stderr, "[msgbox%s] %s: %s\n", getenv("BGI_MSGBOX_STDERR") != NULL ? "" : " auto", utfCap,
			utfText);
		fflush(stderr);
		return ret;
	}
	switch(flags & 0xf0)
	{
	case OS_MB_ICONHAND: type = SDL_MESSAGEBOX_ERROR; break;
	case OS_MB_ICONWARNING: type = SDL_MESSAGEBOX_WARNING; break;
	default: type = SDL_MESSAGEBOX_INFORMATION; break;
	}
	switch(flags & 0x0f)
	{
	case OS_MB_OKCANCEL:
		buttons[0].flags = ((flags & OS_MB_DEFBUTTON2) == 0) ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0;
		buttons[0].buttonid = OS_IDOK;
		buttons[0].text = "OK";
		buttons[1].flags = ((flags & OS_MB_DEFBUTTON2) != 0) ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0;
		buttons[1].buttonid = OS_IDCANCEL;
		buttons[1].text = "Cancel";
		nbuttons = 2;
		break;
	case OS_MB_YESNO:
		buttons[0].flags = ((flags & OS_MB_DEFBUTTON2) == 0) ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0;
		buttons[0].buttonid = OS_IDYES;
		buttons[0].text = "Yes";
		buttons[1].flags = ((flags & OS_MB_DEFBUTTON2) != 0) ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0;
		buttons[1].buttonid = OS_IDNO;
		buttons[1].text = "No";
		nbuttons = 2;
		break;
	case OS_MB_RETRYCANCEL:
		buttons[0].flags = ((flags & OS_MB_DEFBUTTON2) == 0) ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0;
		buttons[0].buttonid = OS_IDRETRY;
		buttons[0].text = "Retry";
		buttons[1].flags = ((flags & OS_MB_DEFBUTTON2) != 0) ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0;
		buttons[1].buttonid = OS_IDCANCEL;
		buttons[1].text = "Cancel";
		nbuttons = 2;
		break;
	default:
		buttons[0].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
		buttons[0].buttonid = OS_IDOK;
		buttons[0].text = "OK";
		nbuttons = 1;
		break;
	}
	memset(&data, 0, sizeof data);
	data.flags = (Uint32)type;
	data.title = utfCap;
	data.message = utfText;
	data.numbuttons = nbuttons;
	data.buttons = buttons;
	if(SDL_ShowMessageBox(&data, &ret) != 0 || ret < 0)
		return MessageBoxDefault(flags);
	return ret;
}
