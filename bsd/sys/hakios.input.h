/*
 * /dev/hakios-touch: touch samples on their way from whoever produces them to the
 * program that turns them into HID events for the window server.
 *
 * Darwin expects touches from Apple's multitouch driver through IOHIDFamily; neither exists
 * here. Instead a producer -- a kernel driver for a real touch controller or for an emulated
 * pointer, or a user program replaying a script -- posts samples, and code of ours running
 * inside backboardd reads them and synthesizes the events. Both sides use the same record.
 *
 *   write(): one or more records (time is ignored; the kernel stamps them)
 *   read():  blocks until there is at least one record, returns as many whole ones as fit
 */
#ifndef _SYS_HAKIOS_INPUT_H_
#define _SYS_HAKIOS_INPUT_H_

#include <stdint.h>

#define HAKIOS_TOUCH_DEVICE    "/dev/hakios-touch"

#define HAKIOS_TOUCH_DOWN      1       /* a finger has landed */
#define HAKIOS_TOUCH_MOVE      2
#define HAKIOS_TOUCH_UP        3       /* it has lifted (x, y: where it was last) */
#define HAKIOS_BUTTON_DOWN     4       /* a hardware button; `finger` says which (HAKIOS_BUTTON_*), x and y are 0 */
#define HAKIOS_BUTTON_UP       5

#define HAKIOS_BUTTON_LOCK     1       /* the side button */
#define HAKIOS_BUTTON_VOLUME_UP 2
#define HAKIOS_BUTTON_VOLUME_DOWN 3
#define HAKIOS_BUTTON_HOME     4

struct hakios_touch {
	uint64_t time;                  /* mach_absolute_time() when the sample was posted */
	uint32_t type;                  /* HAKIOS_TOUCH_* or HAKIOS_BUTTON_DOWN/UP */
	uint32_t finger;                /* which finger, from 0 (or which button) */
	uint32_t x, y;                  /* position on the display, 0 = left/top edge, 65535 = right/bottom edge */
};

#ifdef KERNEL
/* For drivers; not from interrupt context. */
void hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);
/* The same for a sample taken at mach_absolute_time() `when` (0 = now), e.g. the host's own time of a touch. */
void hakios_touch_post_at(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, uint64_t when);
void hakios_input_init(void);
void hakios_virtio_input_init(void);   /* boot-arg virtio_input=1: QEMU's virtio pointer and keyboard */
void hakios_goodix_init(void);         /* boot-arg touchscreen=1: the phone's Goodix or FocalTech touch screen */
void hakios_buttons_init(void);        /* boot-arg buttons=1: the phone's power and volume keys */
void hakios_exynos_keys_init(void);    /* boot-arg exynos_keys=1: the Galaxy S9's keys (GPIO) */
void hakios_s6sy761_init(void);        /* boot-arg s6sy761=1: the Galaxy S9's touch screen (I2C) */
void hakios_s6e3ha8_init(void);        /* boot-arg panel=1: the Galaxy S9's panel brightness (kern.hakios.brightness) */
void hakios_s2mpb02_torch_init(void);  /* boot-arg torch=N on the Galaxy S9: its flashlight (/dev/hakios-torch) */
void hakios_exynos_pcie_init(void);    /* boot-arg wifi_pcie=N on the Galaxy S9: PCIe to the BCM4361, stage 1 */
const char *hakios_bcm4361_module(void); /* the S9's radio module, "murata" or "semco", once Wi-Fi read its OTP; or NULL */
void hakios_bt_console(const char *arg);  /* the USB console's "bt" and "bt scan" (bt=7) */
void hakios_bcm4361_bt_init(void);     /* boot-arg bt=N on the Galaxy S9: the BCM4361's Bluetooth on UART_BT, stage 1 */
void hakios_ssp_init(void);            /* boot-arg shub=N on the Galaxy S9: its sensor hub (sys/hakios.sensors.h) */
void hakios_s2mpb02_torch_quiesce(void); /* loader_return(): the S9's flashlight off, if it is lit */
void hakios_exynos_mfc_init(void);     /* boot-arg mfc=N on the Exynos 9810: its hardware video decoder (hakios.exynos_mfc.c) */
void hakios_exynos_camera_init(void);   /* boot-arg cam_s9=N on the Galaxy S9: its front camera, stage 1 */
void hakios_exynos_camera_quiesce(void); /* loader_return(): the S9's front camera powered down, if it was powered */
void hakios_exynos_tee_init(void);      /* boot-arg tee_probe=N on the Galaxy S9: its secure OS asked (1), started and left there (2) */
void hakios_exynos_iris_init(void);     /* boot-arg iris_s9=N on the Galaxy S9: its iris camera (S5K5F1) and IR LED */
void hakios_exynos_iris_quiesce(void);  /* loader_return(): the S9's iris camera powered down, if it was powered */
void hakios_exynos_pcie_quiesce(void); /* loader_return(): the S9's Wi-Fi chip in reset and unpowered, if it was powered */
int hakios_panel_brightness(int level);        /* 0-255; 0 where a panel driver is built in, started and took it */
void hakios_ufs_init(void);           /* ufs=2 to xnuloader: the phone's storage, read only, through the loader (ufs_shared_pa=) */
void hakios_ufs_report(const char *when); /* where reading the storage has spent its time so far */
bool hakios_ufs_dev(dev_t dev);           /* one of the storage's devices */
int hakios_ufs_read_partition(const char *name, uint64_t offset, uint8_t *dst, uint64_t len, uint64_t *bytes); /* for drivers */
/* a file of one of the phone's ext4 partitions, read only, kept for the run (drivers/storage/hakios.ext4.c): 0 or an errno */
int hakios_partition_file(const char *partition, const char *mount, const char *path, uint64_t max, const uint8_t **data,
    uint64_t *size);
/* a file of one of the RAM disks (drivers/storage/hakios.image.c): fw/<name> is the firmware userland/build.sh put there */
const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
void hakios_battery_init(void);        /* boot-arg battery=1: Qualcomm UEFI charger state shared by xnuloader */
int hakios_battery_temperature(void);  /* the battery's temperature in C, -273 while it is not known */
/* one transfer at a time through the PMIC arbiter for this system (drivers/input/hakios.buttons.c) */
void hakios_spmi_lock(void);
void hakios_spmi_unlock(void);
/*
 * How the battery is, as the audio DSP's battery manager says (drivers/audio/hakios.adsp.c, with adsp=1):
 * handed to drivers/power/hakios.battery.c, which keeps it for the sysctls kern.hakios.battery_* and
 * passes it on to drivers/power/hakios.battery.cpp, the power source that Darwin reads. The structure
 * is repeated there (this header is not seen from iokit/), and `size` tells whether the two agree.
 */
struct hakios_battery_state {
	uint32_t size;                  /* sizeof(struct hakios_battery_state) */
	int32_t percent_hundredths;     /* the charge, 0..10000 */
	int32_t voltage_uv;
	int32_t current_ua;             /* more than 0 while the battery takes charge */
	int32_t temperature_hundredths; /* of a degree Celsius */
	int32_t present;                /* 1 if there is a battery */
	int32_t external;               /* 1 if a charger or a computer is on the USB port */
	int32_t status;                 /* Linux's POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full */
	int32_t charge_uah, full_uah, design_uah, cycles;      /* 0 when not known (yet) */
	int32_t usb_type;               /* Linux's POWER_SUPPLY_USB_TYPE_*: 1 a computer's port, 2 a charger, ... */
	int32_t usb_voltage_uv, usb_current_ua;                 /* what the port gives */
};
void hakios_battery_report(const struct hakios_battery_state *state);
void hakios_scfb_battery_init(void);  /* scfb.addr=: the phone's battery under AVF, from the Scion app (hakios.scfb_battery.c) */
void hakios_max77705_init(void);       /* boot-arg max77705=1: the Galaxy S9's fuel gauge and charger (I2C), read only */
void hakios_net_init(void);            /* en0, if there is something to carry its frames: sys/hakios.net.h */
void hakios_cpufreq_init(void);        /* boot-arg cpufreq=1: the phone's processor at full speed */
void hakios_adreno_init(void);         /* boot-arg adreno=N: the first N steps of waking the phone's GPU */
void hakios_adreno_quiesce(void);      /* on the way back to the loader: the GPU block stopped, out of our memory */
void hakios_adreno_stop(void);         /* before that, where the ending is no panic's: the GPU given back in order */
void hakios_adreno_minute(void);       /* once a minute: a line about how warm the GPU is, if it is in use */
void hakios_mali_init(void);           /* boot-arg mali=N: the first N steps of bringing up the Galaxy S9's Mali-G72 */
void hakios_wifi_init(void);           /* boot-arg wifi=1: the phone's Wi-Fi; wifi_step=N: the first N steps of it */
void hakios_adsp_init(void);           /* boot-arg adsp=1: the phone's audio DSP started and linked; adsp_step=N: the first N steps */
void hakios_mpss_init(void);           /* boot-arg modem=1: the phone's modem started and linked; modem_step=N: the first N steps */
void hakios_wifi_stop(void);           /* where a run ends in order: the Wi-Fi DSP, if it was started, shut down */
int hakios_wifi_say(const char *format, ...) __printflike(1, 2);      /* a line of the Wi-Fi steps' log */
int hakios_wifi_tell(const char *format, ...) __printflike(1, 2);     /* ... one that is said even with wifi=1 */
/* the radio's log kept (hakios.lx.c: what the Linux driver and its bus said, the last 32 KB), and said again with what
 * the driver says of itself now: the USB console's "wifi", and once when the console is first up */
void hakios_wifi_report_add(const char *line);
void hakios_wifi_report(bool first);
uint32_t hakios_wifi_option(const char *name);                         /* a boot-arg's number, or what wifi=1 stands for */
const uint8_t *hakios_wifi_file(const char *name, uint64_t *size);     /* a file of fw/ in the small image, in place */
bool hakios_wlfw_exchange(uint32_t step);                              /* step 15 of wifi_step=N: hakios.wlfw.c */
bool hakios_wlfw_registers(uint64_t *address, uint32_t *size, bool *mission); /* ... and what it learnt for step 16 */
void hakios_wlfw_stop(uint32_t step);                                  /* ... and its end, before the DSP is stopped */
void hakios_wifi_quiesce(void);        /* on the way back to the loader: the Wi-Fi DSP, if it still runs, shut down */
/* ath11k's object and the kernel's side of it (drivers/wifi/lx/hakios.lx.c), and what that asks of the phone's radio */
int hakios_lx_start(void);             /* the driver brought up on the radio as far as it is built: 0 or an errno */
void hakios_lx_stop(void);             /* ... and down again, if it stayed up (a network joined) */
int hakios_lx_prepare(void);           /* before all that: the memory the radio will be shown, taken; 0 or an errno */
void hakios_lx_net_early(void);        /* ... and en0 made on the radio now, if the kernel was given its address */
bool hakios_wifi_dsp_is_up(void);      /* the DSP that runs the radio's firmware has been started and has a link */
const uint8_t *hakios_wifi_driver_file(const char *name, uint64_t *size);     /* a file by the name Linux has for it */
bool hakios_wifi_radio_map(uint64_t physical, uint64_t size, unsigned long long *address); /* memory shown to the radio */
bool hakios_wifi_radio_msi(unsigned long long *address, uint32_t *data, uint32_t *first_spi); /* where the radio raises its interrupts */
bool hakios_wifi_radio_address(uint8_t *address);      /* the radio's own address (boot-arg wifi_mac), if one was given */
/* the network to join (fw/network of the small image): name and passphrase, buffer sizes in and lengths out */
bool hakios_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length);

/*
 * The GPU kept on (boot-arg adreno_keep=1), for drivers/gpu/hakios.gpu.cpp: see "the GPU kept on"
 * in drivers/gpu/adreno/hakios.adreno.c. One caller at a time.
 */
bool hakios_gpu_ready(void);
bool hakios_gpu_parameter(uint32_t which, uint64_t *value);
uint64_t hakios_gpu_address(uint64_t size);
void hakios_gpu_address_free(uint64_t address, uint64_t size);
void hakios_gpu_space(uint64_t *used, uint32_t *live, uint64_t *largest, unsigned *holes, unsigned *most,
    uint64_t *lost, uint32_t *refused);
extern uint32_t gpu_va;                 /* boot-arg gpu_va: 0 as before, 1 describe the addresses, 2 also guard them */
bool hakios_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void hakios_gpu_mapped(void);
void hakios_gpu_unmap(uint64_t address, uint64_t size);
void hakios_gpu_to_memory(void *start, uint64_t size);
uint32_t hakios_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
bool hakios_gpu_done(uint32_t fence);
uint32_t hakios_gpu_last(void);
void hakios_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7]);
void hakios_gpu_report(const char *why);
bool hakios_gpu_recover(void);
void hakios_gpu_stop(void);

/*
 * The Mali brought up to its last step (mali=9), for drivers/gpu/mali/hakios.mali.cpp: see "what the service that programs
 * talk to uses" in drivers/gpu/mali/hakios.mali.c. hakios_keep(): a line of the kernel's own, said again every minute.
 */
bool hakios_mali_ready(void);
bool hakios_mali_parameter(uint32_t which, uint64_t *value);
uint64_t hakios_mali_address(uint64_t size, bool executable);
bool hakios_mali_map(uint64_t gpu, uint64_t phys);
void hakios_mali_mapped(void);
void hakios_mali_unmap(uint64_t gpu, uint64_t size);
void hakios_mali_tag(uint64_t address, uint32_t kind, uint32_t width, uint32_t height, uint32_t row_bytes,
    uint32_t format, uint32_t offset);
uint32_t hakios_mali_submit(uint64_t chain, bool fragment);
uint64_t hakios_mali_enqueue(uint64_t chain, bool fragment);
bool hakios_mali_wait(uint64_t seq, uint64_t timeout_ns);
void hakios_keep(const char *format, ...) __printflike(1, 2);
#endif

#endif /* _SYS_HAKIOS_INPUT_H_ */
