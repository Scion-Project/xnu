/*
 * Stand-ins for Apple's closed kernel extensions: just enough of each for the stock Darwin
 * user space to work on a machine without Apple hardware. The platform expert
 * (GenericARMPE) starts them once the registry is up.
 *
 *   MiniIOSurface.cpp              IOSurface: pixel buffers shared between processes
 *   MiniIOMobileFramebuffer.cpp    IOMobileFramebuffer: the display (the boot framebuffer)
 *   MiniAppleKeyStore.cpp          AppleKeyStore: the keybag, with a passcode that gates the screen and encrypts nothing (keystore_real=1: a real key bag)
 *   MiniIONetworkStack.cpp         IONetworkStack: what configd's interface namer waits for
 *   MiniIOAESAccelerator.cpp       IOAESAccelerator: AES-CBC in software, keys of the caller's only (UniXNU's SoftwareAES)
 */
#ifndef _IOKIT_MINIDRIVERS_H
#define _IOKIT_MINIDRIVERS_H

#include <IOKit/IOService.h>

void MiniIOSurfaceStart(IOService * provider);
void MiniIOMobileFramebufferStart(IOService * provider);
void MiniAppleKeyStoreStart(void);
void MiniIONetworkStackStart(void);
void MiniIOAESAcceleratorStart(void);

#endif /* _IOKIT_MINIDRIVERS_H */
