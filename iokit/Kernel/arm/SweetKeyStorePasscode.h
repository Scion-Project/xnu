/*
 * A passcode that gates the screen for the built-in AppleKeyStore of SWEET (SweetKeyStore.cpp):
 * a verifier kept in /private/var/keybags/scion-passcode, the lock state and the delays after wrong
 * attempts. Off unless boot-arg sweetspeed&0x800000. It is a screen lock, not data protection: no
 * data is encrypted with the passcode.
 */
#ifndef _IOKIT_ARM_SWEETKEYSTOREPASSCODE_H
#define _IOKIT_ARM_SWEETKEYSTOREPASSCODE_H

#if defined(SWEET)
#include <IOKit/IOUserClient.h>

#define SWEET_SPEED_AKS_PASSCODE        0x800000

/* Called from AppleKeyStore::start: allocates the lock (the record is read at the first selector). */
void sweet_aks_passcode_init(void);

/*
 * The passcode module's first look at a selector. Returns true if it handled it (then *ret is the
 * answer and the scalar/structure outputs are set), false to let SweetKeyStore.cpp answer.
 */
bool sweet_aks_passcode_external(uint32_t selector, IOExternalMethodArguments *args, IOReturn *ret);

/* Provided by SweetKeyStore.cpp. */
IOReturn sweet_aks_struct_out(IOExternalMethodArguments *args, const void *buf, uint32_t len);
int32_t sweet_aks_struct_in(IOExternalMethodArguments *args, uint8_t *buf, uint32_t max);
void sweet_aks_message_clients(uint32_t message);       /* AppleKeyStore::messageClients, once it has started */
bool sweet_aks_notify_state_get(void);
void sweet_aks_notify_state_set(void);
#endif /* SWEET */

#endif /* _IOKIT_ARM_SWEETKEYSTOREPASSCODE_H */
