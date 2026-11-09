/*
 * Scion: stand-in for CoreEntitlements' V2 interface header, which Apple's open-source XNU names but does not ship.
 * The kernel only holds these as pointers it passes through (libkern/libkern/amfi/amfi.h): opaque types are enough.
 */
#ifndef SCION_COREENTITLEMENTS_V2_API_H
#define SCION_COREENTITLEMENTS_V2_API_H

typedef struct CEKernelAPI CEKernelAPI_t;
typedef struct CEContext CEContext_t;

#endif /* SCION_COREENTITLEMENTS_V2_API_H */
