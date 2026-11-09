/*
 * Scion: stand-in for CodeSignature/Entitlements.h, which Apple's open-source XNU includes but does not ship.
 * bsd/kern/kern_exec.c needs the names of the four browser-engine entitlements, as Apple documents them.
 */
#ifndef SCION_CODESIGNATURE_ENTITLEMENTS_H
#define SCION_CODESIGNATURE_ENTITLEMENTS_H

#define kCSWebBrowserHostEntitlement       "com.apple.developer.web-browser-engine.host"
#define kCSWebBrowserGPUEntitlement        "com.apple.developer.web-browser-engine.rendering"
#define kCSWebBrowserNetworkEntitlement    "com.apple.developer.web-browser-engine.networking"
#define kCSWebBrowserWebContentEntitlement "com.apple.developer.web-browser-engine.webcontent"

#endif /* SCION_CODESIGNATURE_ENTITLEMENTS_H */
