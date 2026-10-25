// krkrdevice - composite through the engine's draw device and show it
//
// This drives the chain the engine's own scripts drive: a tTVPDrawDevice (the
// compositor), its primary layer (a tTJSNI_BaseLayer), fills into that layer
// through the engine's render methods, Update() to composite and Show() to put
// the result on a real window.  The desktop back end supplies the four steps the
// device leaves open; everything between them is the engine's.
//
// Usage: krkrdevice [out.bmp]

#include "tjsCommHead.h"
#include "DrawDevice.h"
#include "LayerIntf.h"
#include "LayerBitmapIntf.h"
#include "RenderManager.h"
#include "tvpgl.h"

#include <cstdio>
#include <unistd.h>

tTVPDrawDevice* TVPCreateDrawDeviceDarwin();
bool TVPDrawDeviceDarwinSaveScreen(tTVPDrawDevice* device, const char* path);

int main(int argc, char** argv)
{
	const char* out = argc > 1 ? argv[1] : "device.bmp";
	TVPInitTVPGL();

	tTVPDrawDevice* device = TVPCreateDrawDeviceDarwin();
	if(device == nullptr)
	{
		fprintf(stderr, "krkrdevice: no draw device\n");
		return 1;
	}
	device->SetWindowSize(320, 200);
	device->SetDestRectangle(tTVPRect(0, 0, 319, 199));

	tTJSNI_BaseLayer* primary = device->GetPrimaryLayer();
	if(primary == nullptr)
	{
		fprintf(stderr, "krkrdevice: no primary layer\n");
		return 1;
	}
	tTVPBaseTexture* screen = primary->GetMainImage();
	printf("primary layer: %dx%d\n", (int)screen->GetWidth(), (int)screen->GetHeight());

	// paint through the engine's own fill methods: 0xAARRGGBB in, and it
	// reverses the colour for the internal 0xAABBGGRR
	screen->Fill(tTVPRect(0, 0, (tjs_int)screen->GetWidth() - 1, (tjs_int)screen->GetHeight() - 1),
		0xff3050a0u);
	screen->FillColor(tTVPRect(40, 40, 280, 160), 0x00ffc040u, 255);

	device->Update();
	device->Show();
	printf("composited and presented\n");
	bool ok = TVPDrawDeviceDarwinSaveScreen(device, out);
	printf(ok ? "wrote %s\n" : "cannot write %s\n", out);
	fflush(stdout);
	_exit(ok ? 0 : 1);
}
