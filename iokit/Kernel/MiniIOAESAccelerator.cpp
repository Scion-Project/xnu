/*
 * MiniIOAESAccelerator: IOAESAccelerator, AES-CBC in software, for the iOS clients that open it.
 *
 * Ported from UniXNU's SoftwareAES, https://github.com/UniXNU/SoftwareAES at commit
 * 4a45326da5c2cd6fe6813d79ee43db20805628af (NativeAES.cpp and OriginalAESABI.h), under its licence,
 * BSD-2-Clause-Patent, which is reproduced below and applies to this whole file.
 *
 * What came from there: the class NativeAES (the provider: AES-CBC through the kernel's own AES,
 * libkern/crypto/aes.h, which here is minicrypto's, osfmk/minicrypto/mc_aes.c), its two performAES
 * entry points, the chaining-value handling, its refusal of hardware keys, and its known-answer test
 * (NIST SP 800-38A F.2.1 to F.2.6), run before the service is published. What is new here: SoftwareAES
 * subclasses Apple's own IOAESAccelerator from the IOCryptoAcceleratorFamily kext, which supplies the
 * user client; this kernel has no Apple kext, so IOAESAccelerator is declared here (without the
 * padding that matched Apple's instance layout: nothing outside this file links against it) and so is
 * IOAESAcceleratorUserClient, with the selector and the request layout that iOS 16.5's clients use.
 *
 * The clients (iOS 16.5, 20F66; found by their strings and read from their code):
 *
 *   libFDR.dylib          AMFDR's key derivation, for fdrhelper, mobilerepaird, corerepaird, fusiond and
 *                         ZhuGeService (entitlement com.apple.private.IOAESAccelerator.fdr-key-handle):
 *                         encrypts 64 bytes with hardware key 0x839
 *   MobileBackup          backupd (entitlement iokit-user-client-class IOAESAcceleratorUserClient):
 *                         encrypts "MobileBackup-ID!" twice with hardware key 0x83b
 *   online-auth-agent     encrypts with hardware key 0x83a, the IV a hash of the time
 *   softwareupdated       NRDCryptoEncrypt/Decrypt, hardware key 0x83c
 *   OTATaskingAgent       only reads the service's property "CrashReporter-ID" (20 bytes; published here,
 *                         see NativeAES::start)
 *
 * Every one of them calls selector 1 with the same 0x58-byte structure in and out (struct
 * IOAESRequest below). Every one of them asks for a key that lives in an iPhone's AES engine and is
 * derived from its UID; there is no such key here, and as in SoftwareAES none is made up: a request
 * with a key handle is refused (kIOReturnUnsupported), logged once per key and program. A request
 * that brings its own key (handle 0) is done. Before this driver those clients did not find the
 * service at all ("Could not find kIOAESAcceleratorClass").
 *
 * Boot-arg ioaes=0: no IOAESAccelerator at all, as the kernel was before this driver.
 *
 * ------------------------------------------------------------------------------------------------
 * Copyright (c) 2026, ArmVirtPlatform contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * Subject to the terms and conditions of this license, each copyright holder
 * and contributor hereby grants to those receiving rights under this license
 * a perpetual, worldwide, non-exclusive, no-charge, royalty-free, irrevocable
 * (except for failure to satisfy the conditions of this license) patent
 * license to make, have made, use, offer to sell, sell, import, and otherwise
 * transfer this software, where such license applies only to those patent
 * claims, already acquired or hereafter acquired, licensable by such copyright
 * holder or contributor that are necessarily infringed by:
 *
 * (a) their Contribution(s) (the licensed copyrights of copyright holders and
 *     non-copyrightable additions of contributors, in source or binary form)
 *     alone; or
 *
 * (b) combination of their Contribution(s) with the work of authorship to
 *     which such Contribution(s) was added by such copyright holder or
 *     contributor, if, at the time the Contribution is added, such addition
 *     causes such combination to be necessarily infringed. The patent license
 *     shall not apply to any other combinations which include the
 *     Contribution.
 *
 * Except as expressly stated above, no rights or licenses from any copyright
 * holder or contributor is granted under this license, whether expressly, by
 * implication, estoppel or otherwise.
 *
 * DISCLAIMER
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/crypto/aes.h>
#include <libkern/crypto/sha1.h>
#include <IOKit/IODeviceTreeSupport.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextern-c-compat"
#include <libkern/crypto/crypto_internal.h>
#pragma clang diagnostic pop
#include <corecrypto/cc.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>

#include "MiniDrivers.h"

/* ---- IOAESAccelerator's interface, as SoftwareAES's OriginalAESABI.h records it ------------------ */

struct IOAESIV {
	unsigned char bytes[16];
};
struct IOAESKeyData {
	uint32_t      bits;             /* 128, 192 or 256 */
	unsigned char bytes[32];
	uint32_t      selector;         /* 0: the key above; anything else names a key of the AES engine */
};
enum IOAESOperation : unsigned { IOAESEncrypt = 0, IOAESDecrypt = 1 };
using IOAESCompletion = void (*)(void *, int);
static_assert(sizeof(IOAESIV) == 16 && __builtin_offsetof(IOAESKeyData, selector) == 36 && sizeof(IOAESKeyData) == 40,
    "the raw key and IV layout of IOAESAccelerator");

class IOCryptoAccelerator : public IOService
{
	OSDeclareDefaultStructors(IOCryptoAccelerator);
};
OSDefineMetaClassAndStructors(IOCryptoAccelerator, IOService);

/* What user space matches on (IOServiceMatching("IOAESAccelerator")) and what kernel callers call. */
class IOAESAccelerator : public IOCryptoAccelerator
{
	OSDeclareDefaultStructors(IOAESAccelerator);
public:
	virtual const char *getAlgorithmName();
	virtual IOReturn performAES(void *, void *, task_t, unsigned long long, IOAESIV *,
	    IOAESOperation, IOAESKeyData *, IOAESCompletion, void *);
	virtual IOReturn performAES(IOMemoryDescriptor *, IOMemoryDescriptor *, unsigned long long,
	    IOAESIV *, IOAESOperation, IOAESKeyData *, unsigned long long,
	    unsigned long long, IOAESCompletion, void *);
	virtual bool isKeySupported(unsigned);
	virtual unsigned getSupportedOptions();
};
OSDefineMetaClassAndStructors(IOAESAccelerator, IOCryptoAccelerator);

const char *
IOAESAccelerator::getAlgorithmName()
{
	return "AES-CBC";
}

IOReturn
IOAESAccelerator::performAES(void *, void *, task_t, unsigned long long, IOAESIV *, IOAESOperation, IOAESKeyData *,
    IOAESCompletion, void *)
{
	return kIOReturnUnsupported;
}

IOReturn
IOAESAccelerator::performAES(IOMemoryDescriptor *, IOMemoryDescriptor *, unsigned long long, IOAESIV *, IOAESOperation,
    IOAESKeyData *, unsigned long long, unsigned long long, IOAESCompletion, void *)
{
	return kIOReturnUnsupported;
}

bool
IOAESAccelerator::isKeySupported(unsigned)
{
	return false;
}

unsigned
IOAESAccelerator::getSupportedOptions()
{
	return 0;
}

/* ---- NativeAES: SoftwareAES's provider ------------------------------------------------------------ */

/* SoftwareAES's diagnostics (its native_aes_* globals), kept as counters for a debugger */
static uint32_t native_aes_kat_status;          /* 1 passed, 2 failed */
static uint32_t native_aes_interface_status;    /* 1 passed, 2 failed */
static uint32_t native_aes_completed;
static uint32_t native_aes_unsupported_key;

class NativeAES : public IOAESAccelerator
{
	OSDeclareDefaultStructors(NativeAES);
public:
	bool start(IOService *) APPLE_KEXT_OVERRIDE;
	const char *getAlgorithmName() APPLE_KEXT_OVERRIDE { return "AES-CBC"; }
	bool isKeySupported(unsigned selector) APPLE_KEXT_OVERRIDE { return selector == 0; }
	unsigned getSupportedOptions() APPLE_KEXT_OVERRIDE { return 1; }
	IOReturn performAES(void *, void *, task_t, unsigned long long, IOAESIV *,
	    IOAESOperation, IOAESKeyData *, IOAESCompletion, void *) APPLE_KEXT_OVERRIDE;
	IOReturn performAES(IOMemoryDescriptor *, IOMemoryDescriptor *, unsigned long long,
	    IOAESIV *, IOAESOperation, IOAESKeyData *, unsigned long long,
	    unsigned long long, IOAESCompletion, void *) APPLE_KEXT_OVERRIDE;
	/* not SoftwareAES's: the user client's way, the calling task's own addresses through copyin and copyout */
	IOReturn performUserAES(user_addr_t input, user_addr_t output, uint32_t bytes, IOAESIV *iv,
	    IOAESOperation operation, const IOAESKeyData *key);
};
OSDefineMetaClassAndStructors(NativeAES, IOAESAccelerator);

/* No padding, no hidden device keys, no change to the registered crypto modes. */
static int
cbc(const unsigned char *input, unsigned char *output, size_t bytes, unsigned char iv[16], const IOAESKeyData &key,
    IOAESOperation operation)
{
	union {
		aes_encrypt_ctx encrypt;
		aes_decrypt_ctx decrypt;
	} context;
	unsigned char nextIV[16];
	int result;

	if (operation == IOAESDecrypt) {
		memcpy(nextIV, input + bytes - 16, 16);
		result = aes_decrypt_key(key.bytes, (int)(key.bits / 8), &context.decrypt);
		if (!result) {
			result = aes_decrypt_cbc(input, iv, static_cast<unsigned>(bytes / 16), output, &context.decrypt);
		}
	} else {
		result = aes_encrypt_key(key.bytes, (int)(key.bits / 8), &context.encrypt);
		if (!result) {
			result = aes_encrypt_cbc(input, iv, static_cast<unsigned>(bytes / 16), output, &context.encrypt);
		}
		if (!result) {
			memcpy(nextIV, output + bytes - 16, 16);
		}
	}
	if (!result) {
		memcpy(iv, nextIV, 16);
	}
	cc_clear(sizeof(context), &context);
	cc_clear(sizeof(nextIV), nextIV);
	return result;
}

/* NIST SP800-38A F.2.1/F.2.2, four CBC blocks, then split and in-place decryption; F.2.3-F.2.6 one block each. */
static bool
knownAnswerTest(NativeAES *provider)
{
	static const unsigned char input[64] = {
		0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
		0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
		0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
		0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10
	};
	static const unsigned char expected[64] = {
		0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46, 0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d,
		0x50, 0x86, 0xcb, 0x9b, 0x50, 0x72, 0x19, 0xee, 0x95, 0xdb, 0x11, 0x3a, 0x91, 0x76, 0x78, 0xb2,
		0x73, 0xbe, 0xd6, 0xb8, 0xe3, 0xc1, 0x74, 0x3b, 0x71, 0x16, 0xe6, 0x9e, 0x22, 0x22, 0x95, 0x16,
		0x3f, 0xf1, 0xca, 0xa1, 0x68, 0x1f, 0xac, 0x09, 0x12, 0x0e, 0xca, 0x30, 0x75, 0x86, 0xe1, 0xa7
	};
	IOAESKeyData key = { 128, { 0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c }, 0 };
	unsigned char iv[16], output[64];

	for (unsigned i = 0; i < 16; ++i) {
		iv[i] = static_cast<unsigned char>(i);
	}
	bool result = !cbc(input, output, 64, iv, key, IOAESEncrypt) && !memcmp(output, expected, 64) &&
	    !memcmp(iv, expected + 48, 16);
	for (unsigned i = 0; i < 16; ++i) {
		iv[i] = static_cast<unsigned char>(i);
	}
	result = result && !cbc(output, output, 16, iv, key, IOAESDecrypt) &&
	    !cbc(output + 16, output + 16, 48, iv, key, IOAESDecrypt) && !memcmp(output, input, 64) &&
	    !memcmp(iv, expected + 48, 16);
	static const IOAESKeyData wideKeys[2] = {
		{ 192, { 0x8e, 0x73, 0xb0, 0xf7, 0xda, 0x0e, 0x64, 0x52, 0xc8, 0x10, 0xf3, 0x2b, 0x80, 0x90, 0x79, 0xe5,
			 0x62, 0xf8, 0xea, 0xd2, 0x52, 0x2c, 0x6b, 0x7b }, 0 },
		{ 256, { 0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe, 0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
			 0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7, 0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4 }, 0 }
	};
	static const unsigned char wideExpected[2][16] = {
		{ 0x4f, 0x02, 0x1d, 0xb2, 0x43, 0xbc, 0x63, 0x3d, 0x71, 0x78, 0x18, 0x3a, 0x9f, 0xa0, 0x71, 0xe8 },
		{ 0xf5, 0x8c, 0x4c, 0x04, 0xd6, 0xe5, 0xf1, 0xba, 0x77, 0x9e, 0xab, 0xfb, 0x5f, 0x7b, 0xfb, 0xd6 }
	};
	for (unsigned n = 0; n < 2; ++n) {
		for (unsigned i = 0; i < 16; ++i) {
			iv[i] = static_cast<unsigned char>(i);
		}
		result = result && !cbc(input, output, 16, iv, wideKeys[n], IOAESEncrypt) && !memcmp(output, wideExpected[n], 16);
		for (unsigned i = 0; i < 16; ++i) {
			iv[i] = static_cast<unsigned char>(i);
		}
		result = result && !cbc(output, output, 16, iv, wideKeys[n], IOAESDecrypt) && !memcmp(output, input, 16);
	}

	/* the same through both public entry points: descriptors with offsets, a completion, a refused key, a bad range */
	IOBufferMemoryDescriptor *source = IOBufferMemoryDescriptor::withCapacity(128, kIODirectionInOut);
	IOBufferMemoryDescriptor *destination = IOBufferMemoryDescriptor::withCapacity(128, kIODirectionInOut);
	IOAESIV publicIV;
	for (unsigned i = 0; i < 16; ++i) {
		publicIV.bytes[i] = static_cast<unsigned char>(i);
	}
	struct Completion {
		unsigned count;
		int status;
	} completion = {};
	auto callback = [](void *argument, int status) {
		Completion *state = static_cast<Completion *>(argument);
		state->count++;
		state->status = status;
	};
	bool interface = source && destination && source->writeBytes(16, input, 64) == 64 &&
	    !provider->performAES(source, destination, 64, &publicIV, IOAESEncrypt, &key, 16, 32, callback, &completion) &&
	    destination->readBytes(32, output, 64) == 64 && !memcmp(output, expected, 64) &&
	    !memcmp(publicIV.bytes, expected + 48, 16) && completion.count == 1 && completion.status == 0;
	key.selector = 2001;    /* a hardware key's selector, deliberately unsupported */
	interface = interface && provider->performAES(source, destination, 64, &publicIV,
	    IOAESEncrypt, &key, 16, 32, callback, &completion) == kIOReturnUnsupported && completion.count == 1 &&
	    destination->readBytes(32, output, 64) == 64 && !memcmp(output, expected, 64) &&
	    !memcmp(publicIV.bytes, expected + 48, 16) && !provider->isKeySupported(key.selector);
	key.selector = 0;
	interface = interface && provider->performAES(source, destination, 64, &publicIV,
	    IOAESEncrypt, &key, 16, 80, callback, &completion) == kIOReturnBadArgument && completion.count == 1;
	for (unsigned i = 0; i < 16; ++i) {
		publicIV.bytes[i] = static_cast<unsigned char>(i);
	}
	interface = interface && !provider->performAES(const_cast<unsigned char *>(input), output, kernel_task,
	    64, &publicIV, IOAESEncrypt, &key, nullptr, nullptr) && !memcmp(output, expected, 64);
	for (unsigned i = 0; i < 16; ++i) {
		publicIV.bytes[i] = static_cast<unsigned char>(i);
	}
	interface = interface && !provider->performAES(output, output, kernel_task, 64, &publicIV,
	    IOAESDecrypt, &key, nullptr, nullptr) && !memcmp(output, input, 64) && !memcmp(publicIV.bytes, expected + 48, 16);
	native_aes_interface_status = interface ? 1 : 2;
	if (source) {
		source->release();
	}
	if (destination) {
		destination->release();
	}
	result = result && interface;
	cc_clear(sizeof(key), &key);
	cc_clear(sizeof(output), output);
	return result;
}

bool
NativeAES::start(IOService *provider)
{
	if (!g_crypto_funcs || !g_crypto_funcs->ccaes_cbc_encrypt || !g_crypto_funcs->ccaes_cbc_decrypt ||
	    !IOService::start(provider)) {
		IOLog("MiniIOAESAccelerator: the kernel has no AES-CBC registered: no IOAESAccelerator\n");
		return false;
	}
	native_aes_kat_status = knownAnswerTest(this) ? 1 : 2;
	if (native_aes_kat_status != 1) {
		IOLog("MiniIOAESAccelerator: the known-answer test failed (interface %u): no IOAESAccelerator\n",
		    native_aes_interface_status);
		return false;
	}
	setProperty("NativeAESSoftwareKeysOnly", true);
	setProperty(kIOUserClientClassKey, "IOAESAcceleratorUserClient");
	/*
	 * OTATaskingAgent's analytics key is the 20 bytes of this property. An iPhone derives them from its UID;
	 * here they are the SHA-1 of the device tree's serial-number (the loader's or avfboot's), the same at every
	 * boot of the same platform and secret from nobody. Without them it says "Value returned for
	 * kCrashReporterPropertyID is not the expected length" and makes up a random key.
	 */
	{
		IORegistryEntry * root = IORegistryEntry::fromPath("/", gIODTPlane);
		OSData * serial = root ? OSDynamicCast(OSData, root->getProperty("serial-number")) : NULL;
		uint8_t digest[SHA1_RESULTLEN];
		SHA1_CTX sha;

		if (serial != NULL && serial->getLength() > 0) {
			SHA1Init(&sha);
			SHA1Update(&sha, serial->getBytesNoCopy(), serial->getLength());
			SHA1Final(digest, &sha);
			setProperty("CrashReporter-ID", digest, sizeof(digest));
		}
		OSSafeReleaseNULL(root);
	}
	setName("IOAESAccelerator");
	registerService();
	IOLog("MiniIOAESAccelerator: IOAESAccelerator up: AES-CBC 128/192/256 in software (libkern's AES), "
	    "known answers passed; software keys only\n");
	return true;
}

IOReturn
NativeAES::performAES(IOMemoryDescriptor *input, IOMemoryDescriptor *output,
    unsigned long long bytes, IOAESIV *iv, IOAESOperation operation,
    IOAESKeyData *key, unsigned long long inputOffset,
    unsigned long long outputOffset, IOAESCompletion callback, void *refcon)
{
	if (!input || !output || !key || !iv || !bytes || (bytes & 15) ||
	    (key->bits != 128 && key->bits != 192 && key->bits != 256)) {
		return kIOReturnBadArgument;
	}
	if (key->selector || (operation != IOAESEncrypt && operation != IOAESDecrypt)) {
		__atomic_fetch_add(&native_aes_unsupported_key, 1, __ATOMIC_RELAXED);
		return kIOReturnUnsupported;
	}
	if (inputOffset > input->getLength() || bytes > input->getLength() - inputOffset ||
	    outputOffset > output->getLength() || bytes > output->getLength() - outputOffset) {
		return kIOReturnBadArgument;
	}
	unsigned char *buffer = static_cast<unsigned char *>(IOMalloc(4096));
	if (!buffer) {
		return kIOReturnNoMemory;
	}
	IOReturn result = input->prepare();
	if (result) {
		IOFree(buffer, 4096);
		return result;
	}
	result = output->prepare();
	if (result) {
		input->complete();
		IOFree(buffer, 4096);
		return result;
	}
	unsigned char chaining[16];
	memcpy(chaining, iv->bytes, 16);
	for (unsigned long long done = 0; done < bytes;) {
		size_t count = static_cast<size_t>(bytes - done > 4096 ? 4096 : bytes - done);
		if (input->readBytes(inputOffset + done, buffer, count) != count ||
		    cbc(buffer, buffer, count, chaining, *key, operation) ||
		    output->writeBytes(outputOffset + done, buffer, count) != count) {
			result = kIOReturnIOError;
			break;
		}
		done += count;
	}
	if (!result) {
		memcpy(iv->bytes, chaining, 16);
		__atomic_fetch_add(&native_aes_completed, 1, __ATOMIC_RELAXED);
	}
	cc_clear(4096, buffer);
	IOFree(buffer, 4096);
	cc_clear(sizeof(chaining), chaining);
	output->complete();
	input->complete();
	if (callback) {
		callback(refcon, result);
		return kIOReturnSuccess;
	}
	return result;
}

IOReturn
NativeAES::performAES(void *input, void *output, task_t task, unsigned long long bytes,
    IOAESIV *iv, IOAESOperation operation, IOAESKeyData *key,
    IOAESCompletion callback, void *refcon)
{
	if (!task || !input || !output || !bytes) {
		return kIOReturnBadArgument;
	}
	IOMemoryDescriptor *source = IOMemoryDescriptor::withAddressRange(
		reinterpret_cast<mach_vm_address_t>(input), bytes, kIODirectionOut, task);
	IOMemoryDescriptor *destination = IOMemoryDescriptor::withAddressRange(
		reinterpret_cast<mach_vm_address_t>(output), bytes, kIODirectionIn, task);
	if (!source || !destination) {
		if (source) {
			source->release();
		}
		if (destination) {
			destination->release();
		}
		return kIOReturnNoMemory;
	}
	IOReturn result = performAES(source, destination, bytes, iv, operation, key, 0, 0, callback, refcon);
	source->release();
	destination->release();
	return result;
}

/*
 * The same as performAES above, for the user client, which runs on the caller's thread: the text is
 * moved with copyin and copyout through the same 4 KB bounce buffer instead of a memory descriptor
 * over the caller's range. The descriptor's way read a page of the caller's that had not been touched
 * yet (aes_test's constant plain text, in its __TEXT, on the small image) as zeros: prepare() wired it
 * without bringing its contents in. A fault taken by copyin brings them in like any other.
 */
IOReturn
NativeAES::performUserAES(user_addr_t input, user_addr_t output, uint32_t bytes, IOAESIV *iv,
    IOAESOperation operation, const IOAESKeyData *key)
{
	if (!input || !output || !key || !iv || !bytes || (bytes & 15) ||
	    (key->bits != 128 && key->bits != 192 && key->bits != 256)) {
		return kIOReturnBadArgument;
	}
	if (key->selector || (operation != IOAESEncrypt && operation != IOAESDecrypt)) {
		__atomic_fetch_add(&native_aes_unsupported_key, 1, __ATOMIC_RELAXED);
		return kIOReturnUnsupported;
	}
	unsigned char *buffer = static_cast<unsigned char *>(IOMalloc(4096));
	if (!buffer) {
		return kIOReturnNoMemory;
	}
	IOReturn result = kIOReturnSuccess;
	unsigned char chaining[16];
	memcpy(chaining, iv->bytes, 16);
	for (uint32_t done = 0; done < bytes;) {
		size_t count = bytes - done > 4096 ? 4096 : bytes - done;
		if (copyin(input + done, buffer, count) != 0) {
			result = kIOReturnBadArgument;
			break;
		}
		if (cbc(buffer, buffer, count, chaining, *key, operation)) {
			result = kIOReturnIOError;
			break;
		}
		if (copyout(buffer, output + done, count) != 0) {
			result = kIOReturnBadArgument;
			break;
		}
		done += count;
	}
	if (!result) {
		memcpy(iv->bytes, chaining, 16);
		__atomic_fetch_add(&native_aes_completed, 1, __ATOMIC_RELAXED);
	}
	cc_clear(4096, buffer);
	IOFree(buffer, 4096);
	cc_clear(sizeof(chaining), chaining);
	return result;
}

/* ---- IOAESAcceleratorUserClient: what user space calls --------------------------------------------- */

/*
 * Selector 1 (kIOAESAcceleratorPerform): this structure in, and the same back out (0x58 bytes both ways in
 * every client). The text, the IV, the operation and the key are SoftwareAES's IOAESIV, IOAESOperation and
 * IOAESKeyData, laid side by side; the pointers are the caller's own addresses.
 */
struct IOAESRequest {
	uint64_t       cleartext;       /* 0x00 the plain text: read when encrypting, written when decrypting */
	uint64_t       ciphertext;      /* 0x08 the cipher text: written when encrypting, read when decrypting */
	uint32_t       size;            /* 0x10 bytes, a multiple of 16 */
	IOAESIV        iv;              /* 0x14 in: the IV; out: the chaining value after the last block */
	IOAESOperation operation;       /* 0x24 0 encrypt, 1 decrypt */
	IOAESKeyData   key;             /* 0x28 bits, the key, its selector (the clients' "key handle") */
	uint32_t       reserved[2];     /* 0x50 zero in every client */
};
static_assert(__builtin_offsetof(IOAESRequest, size) == 0x10 && __builtin_offsetof(IOAESRequest, iv) == 0x14 &&
    __builtin_offsetof(IOAESRequest, operation) == 0x24 && __builtin_offsetof(IOAESRequest, key) == 0x28 &&
    __builtin_offsetof(IOAESRequest, key.selector) == 0x4c && sizeof(IOAESRequest) == 0x58,
    "the request of iOS 16.5's IOAESAccelerator clients");

enum {
	kIOAESAcceleratorPerform = 1,
};
#define kIOAESMaxRequest        (16u << 20)     /* the clients ask for 16 to 64 bytes */

static uint32_t gAESLoggedSelectors;            /* selectors < 32 already reported */
static struct {
	uint32_t selector;
	char     program[20];
} gAESLoggedKeys[16];                           /* (key, program) pairs already reported */
static unsigned gAESLoggedKeyCount;
static IOLock * gAESLogLock;

class IOAESAcceleratorUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOAESAcceleratorUserClient);

	NativeAES * fProvider;

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(IOAESAcceleratorUserClient, IOUserClient);

bool
IOAESAcceleratorUserClient::start(IOService * provider)
{
	fProvider = OSDynamicCast(NativeAES, provider);
	return fProvider != NULL && IOUserClient::start(provider);
}

IOReturn
IOAESAcceleratorUserClient::clientClose(void)
{
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

/* A hardware key, once per key and program: which client asked for what is the useful part of the log. */
static void
logHardwareKey(uint32_t selector)
{
	char program[20] = "?";
	bool seen = false;

	proc_selfname(program, sizeof(program));
	if (gAESLogLock != NULL) {
		IOLockLock(gAESLogLock);
	}
	for (unsigned i = 0; i < gAESLoggedKeyCount && !seen; i++) {
		seen = gAESLoggedKeys[i].selector == selector && strncmp(gAESLoggedKeys[i].program, program, sizeof(program)) == 0;
	}
	if (!seen && gAESLoggedKeyCount < sizeof(gAESLoggedKeys) / sizeof(gAESLoggedKeys[0])) {
		gAESLoggedKeys[gAESLoggedKeyCount].selector = selector;
		strlcpy(gAESLoggedKeys[gAESLoggedKeyCount].program, program, sizeof(gAESLoggedKeys[0].program));
		gAESLoggedKeyCount++;
	} else {
		seen = true;
	}
	if (gAESLogLock != NULL) {
		IOLockUnlock(gAESLogLock);
	}
	if (!seen) {
		IOLog("MiniIOAESAccelerator: %s[%d] asked for hardware key 0x%x: none here (software keys only)\n",
		    program, proc_selfpid(), selector);
	}
}

IOReturn
IOAESAcceleratorUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	IOAESRequest request;
	IOReturn ret;

	if (selector != kIOAESAcceleratorPerform) {
		if (selector < 32 && !(__atomic_fetch_or(&gAESLoggedSelectors, 1u << selector, __ATOMIC_RELAXED) & (1u << selector))) {
			char program[20] = "?";

			proc_selfname(program, sizeof(program));
			IOLog("MiniIOAESAccelerator: %s[%d] selector %u not implemented (structure %u in, %u out)\n",
			    program, proc_selfpid(), selector, args->structureInputSize, args->structureOutputSize);
		}
		return kIOReturnUnsupported;
	}
	if (args->structureInput == NULL || args->structureInputSize < sizeof(request) ||
	    (args->structureOutput != NULL && args->structureOutputSize < sizeof(request))) {
		return kIOReturnBadArgument;
	}
	memcpy(&request, args->structureInput, sizeof(request));
	if (request.key.selector != 0) {
		logHardwareKey(request.key.selector);
		ret = kIOReturnUnsupported;
	} else if (request.size == 0 || (request.size & 15) || request.size > kIOAESMaxRequest ||
	    request.cleartext == 0 || request.ciphertext == 0) {
		ret = kIOReturnBadArgument;
	} else {
		bool encrypt = request.operation == IOAESEncrypt;

		/* the addresses are the calling task's: this runs on the caller's thread */
		ret = fProvider->performUserAES(encrypt ? request.cleartext : request.ciphertext,
		    encrypt ? request.ciphertext : request.cleartext, request.size, &request.iv, request.operation, &request.key);
	}
	if (ret == kIOReturnSuccess && args->structureOutput != NULL) {
		cc_clear(sizeof(request.key.bytes), request.key.bytes);         /* the key does not need to go back */
		memcpy(args->structureOutput, &request, sizeof(request));
		args->structureOutputSize = sizeof(request);
	}
	cc_clear(sizeof(request), &request);
	return ret;
}

/* ---- start ----------------------------------------------------------------------------------------- */

void
MiniIOAESAcceleratorStart(void)
{
	uint32_t enabled = 1;
	IOService * resources;
	NativeAES * aes;

	if (PE_parse_boot_argn("ioaes", &enabled, sizeof(enabled)) && enabled == 0) {
		IOLog("MiniIOAESAccelerator: off (boot-arg ioaes=0)\n");
		return;
	}
	gAESLogLock = IOLockAlloc();
	/* user space finds it by class (IOServiceMatching("IOAESAccelerator")), here IOService:/IOResources/IOAESAccelerator */
	resources = IOService::getResourceService();
	aes = OSTypeAlloc(NativeAES);
	if (resources == NULL || aes == NULL || !aes->init() || !aes->attach(resources)) {
		OSSafeReleaseNULL(aes);
		IOLog("MiniIOAESAccelerator: cannot create the service\n");
		return;
	}
	if (!aes->start(resources)) {
		aes->detach(resources);
	}
	aes->release();
}
