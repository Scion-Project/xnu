/*
 * The GPU as Metal sees it (after IOS-26-Hot xnu patch 0096, MIT). Metal.framework
 * (MTLRegisterDevices) iterates IOServiceMatching("IOAcceleratorES"), loads
 * /System/Library/Extensions/<MetalPluginName>.bundle, takes the class named
 * MetalPluginClassName (a subclass of Metal's _MTLDevice) and calls
 * -initWithAcceleratorPort: with the service. StarbiteAccelerator publishes itself under that
 * class name with the properties that point Metal at StarbiteMetal.bundle (tools/metal2:
 * metal2vulkan over PanVK, Mesa's Vulkan driver for the Mali-G72, which reaches the GPU
 * through StarbiteMali).
 *
 * Boot-arg sbmtl=1 publishes it; without it Metal finds no device and CoreAnimation keeps
 * sbca's OpenGL ES renderer.
 */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

class IOAcceleratorES : public IOService {
	OSDeclareDefaultStructors(IOAcceleratorES);
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

class StarbiteAccelerator : public IOAcceleratorES {
	OSDeclareDefaultStructors(StarbiteAccelerator);

public:
	IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(StarbiteAccelerator, IOAcceleratorES);

IOService *
StarbiteAccelerator::probe(IOService *provider, SInt32 *score)
{
	uint32_t on = 0;
	PE_parse_boot_argn("sbmtl", &on, sizeof(on));
	return on ? IOService::probe(provider, score) : NULL;
}

bool
StarbiteAccelerator::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty("MetalPluginName", "StarbiteMetal");
	setProperty("MetalPluginClassName", "HotMTL2Device");
	setProperty("IOGLBundleName", "StarbiteMetal");
	setProperty("model", "Mali-G72");
	registerService();
	IOLog("STARBITE MTL: IOAcceleratorES published (MetalPluginName StarbiteMetal)\n");
	return true;
}
#endif /* ARM64_BOARD_CONFIG_STARLTE */
