#define HAKIOS_SOC_FIXED
/*
 * Starbite iOS 27 (xnu-12377, S-Boot -> shim -> XNU) port of Scion's Galaxy S9 Bluetooth, from Scion-Project/
 * scion-kernel drivers/bluetooth/bcm4361/hakios.bcm4361_bt.c (APSL; HakiOS/Scion), changed only so:
 *   - The patch (.hcd) is not in any image and there is no UFS/ext4 reader in this tree: at bt=6 it is read from the
 *     iOS root filesystem once it is mounted, with vnode_open()/vn_rdwr() (root_file(), below): bt_hcd=<path>
 *     (absolute, or a name in /private/var/hakios/bt/), else /private/var/hakios/bt/bcm4361B2_<module>.hcd with
 *     <module> from bt_module=murata|semco, else murata's then semco's (the log calls that a guess). The driver waits
 *     for the root filesystem and the file up to bt_hcd_wait seconds (default 180) and, with neither, says step 6 is
 *     skipped. The file is the phone owner's own (from the phone's VENDOR partition); none is carried here.
 *   - Scion's hakios_image_file() (the small image's fw/), hakios_partition_file() (VENDOR, EFS) and
 *     hakios_bcm4361_module() (the module as Wi-Fi read it from OTP: there is no radio driver in this tree) are not
 *     here: the patch comes as above, and the phone's own address from EFS is not read, so only bt_addr=<a> sets one
 *     (without it the chip keeps Broadcom's default 43:61:b2:..., as Scion before its EFS reader).
 *   - The USB console's "bt" / "bt scan" is wired to this tree's DWC3 console (hakios.dwc3.c; the stub in
 *     starlte_usbcon.c is gone). kern.hakios.bt_trace hangs off starlte_usbcon.c's kern.hakios node.
 *   - Started from IOKitBSDInit.cpp next to hakios_exynos_pcie_init(); hakios.bt_node.cpp is iokit/Kernel/.
 * On this path S-Boot, not Silicium's UEFI, ran before the kernel: step 1 (reads only) says how S-Boot left UART_BT,
 * its clocks and pins. Default off (no bt= boot-arg: nothing is mapped or written). No PMIC register is written.
 * What was checked here: compiled and linked into the xnu-12377 STARLTE kernel. It has not run on this path.
 */
/*
 * The Galaxy S9's Bluetooth (starlte, Exynos 9810): the Bluetooth half of the Broadcom BCM4361 on its UART. The chip is
 * powered, answers HCI, takes Broadcom's patch (the .hcd file of the user's own phone), is raised to 4 Mbaud with flow
 * control, and then stays up for the USB console ("bt", "bt scan": an inquiry and an LE scan) and for user space, as
 * /dev/cu.bluetooth (a tty carrying H4, what iOS's bluetoothd opens on an iPhone whose controller is on a UART) and a
 * "bluetooth" registry entry with the chip's address (hakios.bt_node.cpp). No host stack in the kernel. Only with boot-arg
 * bt=N on this phone (the SoC's registers at 0x10000000); nothing is touched without it. The POCO's bt=N is
 * drivers/bluetooth/hakios.bt.c, which is not in the S9's kernel. Seen working on phone 2, 3 November 2026: every step
 * below, the SEMCO module's patch (BCM4347B2 ROM, "BCM4361B2 Crown E32A ANT1 [Version: 0110.0232]" after it), 4 Mbaud,
 * and a scan that heard the LE devices around.
 *
 * What is where (Samsung's device tree for the S9, exynos9810-starlte_eur_open_26.dts of its kernel, and its drivers):
 *   UART_BT      0x10840000  uart@10840000 (dts 9480-9495): Samsung's "uart1", /dev/ttySAC1 to Android
 *                            (init.samsungexynos9810.rc:637), the port bcm43xx.c's low-power code names (BT_UPORT 1,
 *                            bcm43xx.c:46). Not a USI block: its clocks are PERIC1's UART_BT (below), but the serial
 *                            driver treats it as "usi-serial-v2" (USI_CON 0xC4, USI_OPTION 0xC8). A 256-byte FIFO.
 *                            The boot loader leaves it held in reset: USI_CON reads 1 at bt=1 on the phone (as on the
 *                            Tab Active3's mainline port, exynos-linux/samsung-gtactive3wifi 0068/0073), and nothing of
 *                            it works until bt=3 writes USI_CON 1 then 0 and USI_OPTION 2
 *                            Its interrupt (SPI 422) is not used: this polls.
 *   CMU_PERIC1   0x10800000  UART_BT's clocks (cmucal-sfr.c): gates GATE_CLK_PERIC1_UART_BT +0x2034 (1612),
 *                            RSTNSYNC_CLK_PERIC1_UART_BT +0x201C (1571), UART_BT_IPCLK +0x20C4 (1601), UART_BT_PCLK
 *                            +0x20C8 (1541) -- bit 20 manual, bit 21 on (6655-6656); its Q-channel +0x3028 (1641; bit 0
 *                            enable, bit 1 clock request, 6958-6959); its divider DIV_CLK_PERIC1_UART_BT +0x1814 (1577,
 *                            [3:0], 6762) after MUX_CLKCMU_PERIC1_IP_USER +0x0120 (1593, bit 4, 6809). Android leaves
 *                            the divider at 1 from a 400 MHz CLKCMU_PERIC1_IP (cmucal-vclklut.c:533-534, 1905-1907,
 *                            2074-2075): 200 MHz, the rate the baud divisor is worked out from (boot-arg bt_uart_mhz)
 *   the UART's pins  gpq0-0 RXD, -1 TXD, -2 RTS, -3 CTS of ALIVE's GPIO (0x14050000, bank at +0xA0, pinctrl-exynos.c:1934):
 *                            function 2, pull-up (pud 3), "uart1-bus" and "uart1-bus-rxd" (dts 268-282). gpq0-4 and -5
 *                            are the PMIC's SPEEDY bus: nothing here writes their bits
 *   BT_EN        pin 7 (port 0, bit 7) of the PCAL6524 GPIO expander at 0x22 on hsi2c@10930000 (the bluetooth node's first
 *                            gpio, <76 7 0>, dts 13866-13872; pcal6524@22, dts 6767-6786; bcm43xx.c:272 takes it as bt_en).
 *                            This is BT_REG_ON. It is NOT shared with Wi-Fi: WLAN_REG_ON is gpg0-5 of the SoC (bcmdhd's
 *                            dhd_custom_exynos.c:222-236, "cfg_wlanen", dts 2670), driven by drivers/wifi/brcmfmac/hakios.exynos_pcie.c
 *                            (wifi_pcie=2); the BCM4361 has the two lines apart, and bcmdhd's one BT/WLAN REG_ON workaround,
 *                            si_pmu_wlan_bt_reg_on_war() (hndpmu.c:680-710), is for the 4389 alone (PMU rev 40)
 *   the expander's registers  INPUT 0x00, DAT_OUT 0x04, CONFIG 0x0C (+ port; a CONFIG bit 1 is an input), one byte each
 *                            (include/linux/i2c/pcal6524.h:8-11); an output is set by its DAT_OUT bit first, then its
 *                            CONFIG bit cleared (gpio-pcal6524.c, pcal6524_gpio_direction_output()). The same port holds
 *                            the DisplayPort switch (pins 1, 4) and the fingerprint sensor's sleep pin (6); port 1 the
 *                            NFC (9, 11) and wireless charging (8): every write here is a read-modify-write of bit 7 of
 *                            port 0's two registers, read back
 *   the expander's bus  hsi2c@10930000 (dts 6747-6766): USI10_I2C of PERIC1, clocks GATE_USI10_I2C and PERIC1_USI_I2C
 *                            (dt-bindings exynos9810.h:122, 144; clk-exynos9810.c:348, 456): gates GATE_CLK_PERIC1_USI_I2C
 *                            +0x2050 (1613), RSTNSYNC_USI10_I2C +0x20A8 (1592), USI10_I2C_IPCLK +0x210C (1610),
 *                            USI10_I2C_PCLK +0x2110 (1564), Q-channel +0x304C (1650), divider +0x1830 (1578); its input
 *                            clock 200 MHz (the node's default-clk). Pins gpp6-2 and -3 of PERIC1's GPIO, function 3
 *                            ("hsi2c25-bus", dts 2969-2975; gpp6 at +0x40, pinctrl-exynos.c:2001). USI10's mode word
 *                            (SYSREG_PERIC1 0x1081103C, usi@1081103C, dts 5612) is I2C (4, usi_v2.c:18-36) under Android,
 *                            for USI10's own I2C (the flashlight's bus, hakios.s2mpb02_torch.c), which leaves the block's
 *                            pins 2 and 3 to USI10_I2C; the word USI10_I2C would have, 0x10811040, is disabled in the tree
 *                            and not written. The bus runs at 100 kHz.
 *   the expander's reset  gpg3-6 of PERIC1's GPIO (0x10830000, bank at +0xC0, pinctrl-exynos.c:2005): output, high to run
 *                            ("expander_reset_active", dts 3405-3412; Android pulses it at probe, gpio-pcal6524.c:418-445)
 *   BT_WAKE      gpg2-0 of PERIC0's GPIO (0x10430000, +0xC0, pinctrl-exynos.c:1994): output, the host's "stay awake" to
 *                            the chip ("bt-btwake", dts 2691-2699), set low at probe and high whenever the UART sends
 *                            (bcm43xx.c:318, 131)
 *   BT_HOST_WAKE gpa2-3 of ALIVE's GPIO (+0x60, pinctrl-exynos.c:1932): the chip's to the host, read only here
 *                            ("bt-hostwake", dts 531-537)
 *   UART registers (include/linux/serial_s3c.h:32-49, 111, 170, 214-267): ULCON 0x00, UCON 0x04 (bit 5 loopback, [3:2] and
 *                            [1:0] the TX and RX modes, 1 = by the CPU), UFCON 0x08 (bit 0 FIFO on, 1 and 2 reset RX
 *                            and TX), UMCON 0x0C (bit 4 auto flow control), UTRSTAT 0x10, UERSTAT 0x14, UFSTAT 0x18 (RX
 *                            count [7:0], RX full bit 8, TX count [23:16], TX full bit 24), UMSTAT 0x1C (bit 0 CTS),
 *                            UTXH 0x20, URXH 0x24, UBRDIV 0x28, UFRACVAL 0x2C, UINTP 0x30, UINTSP 0x34, UINTM 0x38. USI_CON
 *                            is written 1 then 0, USI_OPTION 2 (CLKREQ on), all interrupts masked and cleared
 *                            (samsung.c:1555-1571, 1730-1737). The divisor: hakios.bcm_hci.h, as samsung.c works it out.
 *
 * What the chip is told, as Linux's Broadcom drivers tell it (btbcm.c, hci_bcm.c of the S9's kernel): it starts at 115200
 * baud, 8N1, with RTS/CTS (hci_bcm.c:817 init_speed; samsung.c:1268-1274); HCI_Reset (0x0C03, btbcm.c:173) and Read Local
 * Version (0x1001, 209); the patch: Download Minidriver (0xFC2E), 50 ms, the .hcd's records sent one by one as commands,
 * each answered before the next, then 250 ms after the last, Launch_RAM (btbcm.c:106-167); then HCI_Reset and Read Local
 * Version again; the patch goes at 115200 (108 KB, about ten seconds). Then (bt=7) the rate: hci_bcm.c's bcm_set_baudrate()
 * sends 0xFC45 (the UART clock at 48 MHz) above 3 Mbaud, which this module's BCM4347B2 refuses (status 01, unknown
 * command) and does not need: 0xFC18 with 4000000, answered at the old rate, then the UART to 4 Mbaud (UBRDIV 2,
 * UFRACVAL 2 from 200 MHz: exact). BT_EN high and 100 ms before the first byte (bcm43xx.c:96-99); low to power it off (112).
 *
 *   bt=1   a look, reading only: UART_BT's clocks and pins, BT_WAKE, BT_HOST_WAKE, the expander's reset pin and bus as
 *          found; UART_BT's registers if its clocks are open (a block whose clock is off can stop the bus)
 *   bt=2   ... the expander's bus (its clocks on, USI10 to I2C, its two pins), and the expander read: its three ports' input,
 *          output and direction, BT_EN's among them. With the expander held in reset (gpg3-6 low) it stops there, unless
 *          bt_expander_reset=1, which lets it go as Android's pinctrl does (gpg3-6 output high)
 *   bt=3   ... UART_BT set up (clocks, pins, 115200 8N1 from its clock) and tried in its internal loopback: 16 bytes out
 *          and back with no chip involved; then flow control on
 *   bt=4   ... the chip powered: BT_WAKE high, BT_EN output low (Android's probe), 10 ms, BT_EN high, 100 ms; each expander
 *          write read back. What the chip says on its own (nothing is expected), BT_HOST_WAKE and CTS logged
 *   bt=5   ... HCI over H4: HCI_Reset, Read Local Version (Broadcom is manufacturer 0x000F), Read Local Name: the chip's ROM
 *   bt=6   ... the patch: in this port a file on the iOS root filesystem (the note at the top; Scion takes fw/ of its small
 *          image or the phone's VENDOR: bcm4361B2_murata.hcd or _semco.hcd, whichever its module is, BLUETOOTH.md),
 *          read through and checked first (hakios.bcm_hci.h), then sent; HCI_Reset, Read Local Version and Name again:
 *          the LMP subversion and the name now the patch's
 *   bt=7   ... the rate raised (bt_baud=, default 4000000; if the chip does not answer at it, it is started and patched
 *          again and kept at 115200), the address read, and the chip kept powered: the USB console's "bt" (its state) and
 *          "bt scan" (Set Event Mask with LE Meta, Write Inquiry Mode 2, a general inquiry of 7.7 s, an active LE scan of
 *          5 s, every device heard listed once, with its RSSI and name), /dev/cu.bluetooth (also uart.bluetooth and
 *          tty.bluetooth; bt_tty=0 leaves them out), the kernel control com.apple.uart.bluetooth (the UART as iOS's
 *          BlueTool and bluetoothd open it) and the "bluetooth" registry entry for iOS's bluetoothd
 *   bt_scan=1        at bt=7, one scan as soon as the chip is up, before the console has it
 *   bt_addr=<a>      at bt=7, the chip's address, "8c:83:e1:02:ee:e9" (in Scion the default is the phone's own from
 *                    its EFS partition; this port has no EFS reader, so without bt_addr= the chip keeps Broadcom's
 *                    default 43:61:b2:..., the same in every S9)
 *   bt_trace=N       at bt=7, the H4 packets on com.apple.uart.bluetooth, both ways, on the console: at most N lines a
 *                    second (each: the direction, the type, the opcode, event or handle, the length, the first bytes);
 *                    the packets beyond that are counted and the count said with the next line
 *   bt_keep=1        leave the chip powered at the end (default: BT_EN and BT_WAKE low once the steps are done or one fails)
 *   bt_hcd=<path>    (this port) the patch file on the root filesystem, absolute or a name in /private/var/hakios/bt/
 *   bt_module=<m>    murata or semco: /private/var/hakios/bt/bcm4361B2_<m>.hcd (default: murata's, then semco's: a guess)
 *   bt_hcd_wait=<s>  (this port) seconds to wait for the root filesystem and the file at bt=6 (default 180)
 *   bt_expander_reset=1  at bt=2 and up, take the expander out of reset if it is held there
 *   bt_uart_mhz=<n>  UART_BT's clock in MHz, if bt=1 shows the divider or mux not as Android leaves them (default 200)
 * Each step says what it is about to do before it does it.
 *
 * What was checked: compiled, with the driver on and off; every register, bit and number above against the BSP files
 * named with them; the H4 framing, the events, the .hcd reading and the divisor on a computer by tools/bcm_hci_test.c
 * (tools/bcm-hci-test.sh); and on phone 2 (SEMCO module), bt=7 end to end. Not in QEMU (which has no such UART or
 * expander: bt=N must not be given there).
 *
 * Sources (Samsung's GPL kernel for the S9, Scion-Project/android_kernel_samsung_exynos9810 at e5906667068e, read for
 * these facts, not copied): drivers/bluetooth/bcm43xx.c, btbcm.c, hci_bcm.c, include/net/bluetooth/hci.h,
 * drivers/tty/serial/samsung.c, include/linux/serial_s3c.h, drivers/gpio/gpio-pcal6524.c, include/linux/i2c/pcal6524.h,
 * drivers/pinctrl/samsung/pinctrl-exynos.c, drivers/soc/samsung/usi_v2.c, drivers/soc/samsung/cal-if/exynos9810/
 * (cmucal-sfr.c, cmucal-vclklut.c, cmucal-node.c), drivers/clk/samsung/clk-exynos9810.c, include/dt-bindings/clock/
 * exynos9810.h, drivers/net/wireless/broadcom/bcmdhd_101_12/dhd_custom_exynos.c and hndpmu.c,
 * arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts; LineageOS's device_samsung_exynos9810-common
 * (init.samsungexynos9810.rc, proprietary-files.txt: the two .hcd files).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/OSAtomic.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/tty.h>
#include <sys/ttycom.h>
#include <miscfs/devfs/devfs.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/sys_domain.h>
#include <sys/sysctl.h>
#include <sys/hakios.exynos_soc.h>
#include "hakios.bcm_hci.h"
#include <sys/hakios.log.h>
#include <sys/hakios.xnu.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <kern/kalloc.h>

extern void HakiosBTNodeStart(const uint8_t *hci_address, uint32_t speed);

/* ---- where ---------------------------------------------------------------------------------------------------- */

#define UART_PA                 EXYNOS_UART_BT_PA
#define CMU_PERIC1_PA           EXYNOS_CMU_PERIC1_PA
#define CMU_PERIC1_SIZE         0x4000UL
#define GPIO_ALIVE_PA           EXYNOS_GPIO_ALIVE_PA
#define GPIO_PERIC0_PA          EXYNOS_GPIO_PERIC0_PA
#define GPIO_PERIC1_PA          EXYNOS_GPIO_PERIC1_PA

/* UART_BT's clocks in CMU_PERIC1 */
#define GATE_MANUAL             (1u << 20)
#define GATE_ON                 (1u << 21)
static const struct { uint16_t off; const char *name; } uart_gates[] = {
	{ 0x2034, "GATE_CLK_UART_BT" }, { 0x201C, "RSTNSYNC" }, { 0x20C4, "IPCLK" }, { 0x20C8, "PCLK" },
};
#define UART_QCH                0x3028
#define UART_DIV                0x1814
#define PERIC1_IP_USER_MUX      0x0120  /* bit 4: 1 = CLKCMU_PERIC1_IP from CMU_TOP, 0 = the oscillator */

/* the UART */
#define ULCON                   0x00
#define UCON                    0x04
#define  UCON_LOOPBACK          (1u << 5)
#define  UCON_CPU_MODES         0x5u    /* TX [3:2] and RX [1:0] by the CPU (interrupt or polling) */
#define UFCON                   0x08
#define  UFCON_FIFO             (1u << 0)
#define  UFCON_RESET_BOTH       (3u << 1)
#define UMCON                   0x0C
#define  UMCON_AFC              (1u << 4)
#define UTRSTAT                 0x10
#define UERSTAT                 0x14
#define UFSTAT                  0x18
#define  UFSTAT_RX_COUNT        0xffu
#define  UFSTAT_RX_FULL         (1u << 8)
#define  UFSTAT_TX_FULL         (1u << 24)
#define  UFSTAT_TX_COUNT(v)     (((v) >> 16) & 0xffu)
#define UMSTAT                  0x1C
#define  UMSTAT_CTS             (1u << 0)
#define UTXH                    0x20
#define URXH                    0x24
#define UBRDIV                  0x28
#define UFRACVAL                0x2C
#define UINTP                   0x30
#define UINTSP                  0x34
#define UINTM                   0x38
#define USI_CON                 0xC4
#define USI_OPTION              0xC8
#define  USI_CLKREQ_ON          (1u << 1)
#define ULCON_8N1               0x3u
#define BAUD                    115200

/* pins: banks of 4-bit CON (+0), 1-bit DAT (+4), 4-bit PUD (+8) */
#define BANK_GPQ0               0x0A0   /* ALIVE */
#define BANK_GPA2               0x060   /* ALIVE */
#define BANK_GPG2               0x0C0   /* PERIC0 */
#define BANK_GPG3               0x0C0   /* PERIC1 */
#define BANK_GPP6               0x040   /* PERIC1 */
#define PIN_BT_WAKE             0       /* gpg2 */
#define PIN_HOST_WAKE           3       /* gpa2 */
#define PIN_EXP_RESET           6       /* gpg3 */
#define UART_PINS_MASK          0xffffu /* gpq0-0..3; -4 and -5 are SPEEDY's */
#define UART_PINS_CON           0x2222u
#define UART_PINS_PUD           0x3333u

/* the expander */
#define EXPANDER                0x22
#define PCAL_INPUT              0x00
#define PCAL_DAT_OUT            0x04
#define PCAL_CONFIG             0x0C
#define BT_EN_BIT               7       /* port 0 */

#define BT_POWER_MS             100     /* bcm43xx.c:99 */
#define MINIDRV_MS              50      /* btbcm.c:126 */
#define LAUNCH_MS               250     /* btbcm.c:162 */
#define CMD_TIMEOUT_MS          3000

static struct hakios_hsi2c bus = {
	.name = "bt-i2c",
	.regs_pa = EXYNOS_PERIC1_USI10_HSI2C1_PA,                  /* hsi2c@10930000: USI10_I2C of PERIC1 */
	.usi_mode_pa = EXYNOS_PERIC1_USI10_MODE_PA,              /* USI10's mode word: I2C, as Android leaves it (the header) */
	.cmu_pa = CMU_PERIC1_PA, .cmu_size = CMU_PERIC1_SIZE,
	.gates = { 0x2050, 0x20A8, 0x210C, 0x2110 }, .ngates = 4,
	.qch = 0x304C, .div = 0x1830, .mux = 0x0120,
	.pins_pa = GPIO_PERIC1_PA + BANK_GPP6,  /* gpp6-2 SCL, gpp6-3 SDA: [15:8] */
	.pins_mask = 0xff00u, .pins_want = 0x3300u,
	.ipclk_mhz = 200, .bus_hz = 100000,
};

static vm_offset_t uart, cmu, alive, peric0, peric1;
static uint32_t uart_mhz = 200;
static bool powered;                    /* BT_EN driven high by this run */
static bool i2c_up;
static struct h4_rx rx;
static uint32_t rx_errors;
static bool accept_status;              /* hci_start(): a Command Status of 0 ends the wait */
static void scan_event(const uint8_t *event);
static bool scanning;

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
deadline_ms(uint32_t ms)
{
	uint64_t interval;

	nanoseconds_to_absolutetime((uint64_t)ms * NSEC_PER_MSEC, &interval);
	return mach_absolute_time() + interval;
}

/* ---- pins ---------------------------------------------------------------------------------------------------- */

static uint32_t
pin_con(vm_offset_t gpio, uint32_t bank, unsigned pin)
{
	return (rd(gpio, bank) >> (4 * pin)) & 0xf;
}

static uint32_t
pin_dat(vm_offset_t gpio, uint32_t bank, unsigned pin)
{
	return (rd(gpio, bank + 4) >> pin) & 1;
}

/* an output at a level: the level first, then the function (so it never drives the other one) */
static void
pin_output(vm_offset_t gpio, uint32_t bank, unsigned pin, bool high)
{
	uint32_t dat = rd(gpio, bank + 4), con = rd(gpio, bank);

	wr(gpio, bank + 4, high ? dat | 1u << pin : dat & ~(1u << pin));
	wr(gpio, bank, (con & ~(0xfu << (4 * pin))) | 1u << (4 * pin));
}

static void
look_pins(const char *when)
{
	say("bt: %s: UART pins gpq0 CON %08x PUD %08x DAT %02x (RXD TXD RTS CTS want function 2, pull-up: [15:0] %04x, %04x)\n",
	    when, rd(alive, BANK_GPQ0), rd(alive, BANK_GPQ0 + 8), rd(alive, BANK_GPQ0 + 4), UART_PINS_CON, UART_PINS_PUD);
	say("bt: %s: BT_WAKE gpg2-0 function %x level %u; BT_HOST_WAKE gpa2-3 function %x level %u; expander reset gpg3-6 "
	    "function %x level %u (1, 1 is running)\n", when, pin_con(peric0, BANK_GPG2, PIN_BT_WAKE),
	    pin_dat(peric0, BANK_GPG2, PIN_BT_WAKE), pin_con(alive, BANK_GPA2, PIN_HOST_WAKE),
	    pin_dat(alive, BANK_GPA2, PIN_HOST_WAKE), pin_con(peric1, BANK_GPG3, PIN_EXP_RESET),
	    pin_dat(peric1, BANK_GPG3, PIN_EXP_RESET));
}

/* ---- UART_BT ------------------------------------------------------------------------------------------------- */

/* a gate is open while hardware runs it (manual clear) or it is forced on */
static bool
uart_clocks_open(void)
{
	bool open = true;

	for (unsigned i = 0; i < sizeof(uart_gates) / sizeof(uart_gates[0]); i++) {
		uint32_t g = rd(cmu, uart_gates[i].off);

		if ((g & GATE_MANUAL) && !(g & GATE_ON)) {
			open = false;
		}
	}
	return open;
}

static void
look_clocks(void)
{
	uint32_t div = rd(cmu, UART_DIV), mux = rd(cmu, PERIC1_IP_USER_MUX);

	say("bt: UART_BT's clock: PERIC1_IP_USER mux %08x (bit 4: %s), divider %08x (ratio %u: %u MHz if the input is "
	    "Android's 400; %u assumed), Q-channel %08x\n", mux, (mux & (1u << 4)) ? "CMU_TOP's" : "the oscillator, 26 MHz",
	    div, (div & 0xf) + 1, 400 / ((div & 0xf) + 1), uart_mhz, rd(cmu, UART_QCH));
	for (unsigned i = 0; i < sizeof(uart_gates) / sizeof(uart_gates[0]); i++) {
		say("bt:   gate %s %08x\n", uart_gates[i].name, rd(cmu, uart_gates[i].off));
	}
}

static void
look_uart(const char *when)
{
	say("bt: %s: ULCON %x UCON %x UFCON %x UMCON %x UTRSTAT %x UFSTAT %x UMSTAT %x UBRDIV %x UFRACVAL %x UINTM %x "
	    "USI_CON %x USI_OPTION %x\n", when, rd(uart, ULCON), rd(uart, UCON), rd(uart, UFCON), rd(uart, UMCON),
	    rd(uart, UTRSTAT), rd(uart, UFSTAT), rd(uart, UMSTAT), rd(uart, UBRDIV), rd(uart, UFRACVAL), rd(uart, UINTM),
	    rd(uart, USI_CON), rd(uart, USI_OPTION));
}

static void
fifo_reset(void)
{
	uint64_t until = deadline_ms(10);

	wr(uart, UFCON, UFCON_FIFO | UFCON_RESET_BOTH);
	while ((rd(uart, UFCON) & UFCON_RESET_BOTH) && mach_absolute_time() < until) {
	}
	wr(uart, UFCON, UFCON_FIFO);
}

static bool
bt_uart_setup(void)
{
	struct exynos_uart_div d;

	if (!exynos_uart_divisor(uart_mhz * 1000000u, BAUD, &d)) {
		say("bt: 3: no divisor for %u baud from %u MHz\n", BAUD, uart_mhz);
		return false;
	}
	say("bt: 3: UART_BT's clocks forced on and its Q-channel enabled; gpq0-0..3 to the UART, pulled up\n");
	for (unsigned i = 0; i < sizeof(uart_gates) / sizeof(uart_gates[0]); i++) {
		wr(cmu, uart_gates[i].off, rd(cmu, uart_gates[i].off) | GATE_MANUAL | GATE_ON);
	}
	wr(cmu, UART_QCH, rd(cmu, UART_QCH) | 3);
	wr(alive, BANK_GPQ0 + 8, (rd(alive, BANK_GPQ0 + 8) & ~UART_PINS_MASK) | UART_PINS_PUD);
	wr(alive, BANK_GPQ0, (rd(alive, BANK_GPQ0) & ~UART_PINS_MASK) | UART_PINS_CON);
	say("bt: 3: the UART: out of USI reset, interrupts masked, FIFOs on and emptied, 8N1, UBRDIV %u UFRACVAL %u "
	    "(%u baud from %u MHz)\n", d.ubrdiv, d.fracval, d.actual, uart_mhz);
	wr(uart, USI_CON, 1);
	delay(1);
	wr(uart, USI_CON, 0);
	delay(1);
	wr(uart, USI_OPTION, USI_CLKREQ_ON);
	wr(uart, UINTM, 0xf);
	wr(uart, UINTP, 0xf);
	wr(uart, UINTSP, 0xf);
	wr(uart, UCON, 0);
	wr(uart, UMCON, 0);
	fifo_reset();
	wr(uart, ULCON, ULCON_8N1);
	wr(uart, UBRDIV, d.ubrdiv);
	wr(uart, UFRACVAL, d.fracval);
	(void)rd(uart, UERSTAT);
	look_uart("3: set up");
	return true;
}

/* bytes out, waiting while the FIFO is full (with flow control on, while the chip holds CTS) */
static bool
bt_write(const uint8_t *b, size_t n, uint32_t timeout_ms)
{
	uint64_t until = deadline_ms(timeout_ms);

	for (size_t i = 0; i < n; i++) {
		while (rd(uart, UFSTAT) & UFSTAT_TX_FULL) {
			if (mach_absolute_time() > until) {
				say("bt: sending stopped at byte %zu of %zu: UFSTAT %x UMSTAT %x (CTS %s)\n", i, n, rd(uart, UFSTAT),
				    rd(uart, UMSTAT), (rd(uart, UMSTAT) & UMSTAT_CTS) ? "asserted" : "not asserted");
				return false;
			}
		}
		wr(uart, UTXH, b[i]);
	}
	return true;
}

/* one byte in, or -1 when there is none */
static int
bt_getc(void)
{
	uint32_t st = rd(uart, UFSTAT);

	if ((st & UFSTAT_RX_COUNT) == 0 && !(st & UFSTAT_RX_FULL)) {
		return -1;
	}
	if (rd(uart, UERSTAT) & 0xf) {
		rx_errors++;
	}
	return (int)(rd(uart, URXH) & 0xff);
}

static bool
bt_uart_loopback(void)
{
	static const char probe[] = "HakiOS BT loop!";
	uint8_t got[sizeof(probe)];
	size_t n = 0;
	uint64_t until;

	say("bt: 3: the UART's internal loopback (no chip involved): %zu bytes out and back\n", sizeof(probe));
	wr(uart, UCON, UCON_LOOPBACK | UCON_CPU_MODES);
	fifo_reset();
	if (!bt_write((const uint8_t *)probe, sizeof(probe), 100)) {
		wr(uart, UCON, UCON_CPU_MODES);
		return false;
	}
	until = deadline_ms(50);
	while (n < sizeof(probe) && mach_absolute_time() < until) {
		int c = bt_getc();

		if (c >= 0) {
			got[n++] = (uint8_t)c;
		}
	}
	wr(uart, UCON, UCON_CPU_MODES);
	fifo_reset();
	if (n != sizeof(probe) || memcmp(got, probe, sizeof(probe)) != 0) {
		say("bt: 3: loopback: %zu of %zu bytes came back%s: the UART is not running (UFSTAT %x)\n", n, sizeof(probe),
		    n == sizeof(probe) ? ", not the same" : "", rd(uart, UFSTAT));
		return false;
	}
	say("bt: 3: loopback: all %zu bytes came back\n", n);
	return true;
}

/* ---- the expander -------------------------------------------------------------------------------------------- */

static bool
pcal_get(uint8_t reg, uint8_t *value)
{
	uint32_t e = hakios_hsi2c_read_reg(&bus, EXPANDER, reg, value, 1);

	if (e != 0) {
		say("bt: the expander's register %02x could not be read (%x)\n", reg, e);
		return false;
	}
	return true;
}

/* bit 7 of port 0's DAT_OUT or CONFIG, and nothing else; read back */
static bool
pcal_bt_en_bit(uint8_t reg, bool on)
{
	uint8_t now, want, back = 0, buf[2];

	if ((reg != PCAL_DAT_OUT && reg != PCAL_CONFIG) || !pcal_get(reg, &now)) {
		return false;
	}
	want = pcal_with_bit(now, BT_EN_BIT, on);
	buf[0] = reg;
	buf[1] = want;
	if (want != now && hakios_hsi2c_message(&bus, EXPANDER, false, buf, 2, true) != 0) {
		say("bt: the expander's register %02x could not be written\n", reg);
		return false;
	}
	if (!pcal_get(reg, &back) || back != want) {
		say("bt: the expander's register %02x: %02x written over %02x, %02x read back\n", reg, want, now, back);
		return false;
	}
	return true;
}

static bool
look_expander(const char *when)
{
	uint8_t in[3], out[3], cfg[3];

	for (unsigned p = 0; p < 3; p++) {
		if (!pcal_get((uint8_t)(PCAL_INPUT + p), &in[p]) || !pcal_get((uint8_t)(PCAL_DAT_OUT + p), &out[p]) ||
		    !pcal_get((uint8_t)(PCAL_CONFIG + p), &cfg[p])) {
			return false;
		}
	}
	say("bt: %s: the expander: input %02x %02x %02x, output %02x %02x %02x, direction %02x %02x %02x (1 an input); "
	    "BT_EN (P0_7) %s, driving %u, reads %u\n", when, in[0], in[1], in[2], out[0], out[1], out[2], cfg[0], cfg[1], cfg[2],
	    (cfg[0] >> BT_EN_BIT) & 1 ? "an input" : "an output", (out[0] >> BT_EN_BIT) & 1, (in[0] >> BT_EN_BIT) & 1);
	return true;
}

static bool
expander_up(void)
{
	if (!i2c_up) {
		say("bt: 2: the expander's bus: USI10_I2C's clocks on, USI10's mode I2C (Android's), gpp6-2/3 function 3, "
		    "the controller reset, %u kHz\n", bus.bus_hz / 1000);
		hakios_hsi2c_power(&bus);
		hakios_hsi2c_reset(&bus);
		i2c_up = true;
	}
	if (pin_con(peric1, BANK_GPG3, PIN_EXP_RESET) != 1 || pin_dat(peric1, BANK_GPG3, PIN_EXP_RESET) != 1) {
		uint32_t release = 0;

		PE_parse_boot_argn("bt_expander_reset", &release, sizeof(release));
		if (release != 1) {
			say("bt: 2: the expander is held in reset (gpg3-6 not an output high): it is left so; "
			    "bt_expander_reset=1 lets it go as Android does\n");
			return false;
		}
		say("bt: 2: the expander out of reset: gpg3-6 an output, high (bt_expander_reset=1), then 10 ms\n");
		pin_output(peric1, BANK_GPG3, PIN_EXP_RESET, true);
		sleep_ms(10);
	}
	return look_expander("2");
}

/* ---- power --------------------------------------------------------------------------------------------------- */

static void
power_off(const char *why)
{
	if (powered) {
		say("bt: the chip off (%s): BT_EN low, BT_WAKE low\n", why);
		if (!pcal_bt_en_bit(PCAL_DAT_OUT, false)) {
			say("bt: BT_EN could not be brought low: the chip may still be powered until the phone restarts\n");
		}
		powered = false;
	}
	if (peric0 && pin_con(peric0, BANK_GPG2, PIN_BT_WAKE) == 1) {
		pin_output(peric0, BANK_GPG2, PIN_BT_WAKE, false);
	}
}

static bool
power_on(void)
{
	say("bt: 4: BT_WAKE (gpg2-0) an output, high\n");
	pin_output(peric0, BANK_GPG2, PIN_BT_WAKE, true);
	say("bt: 4: BT_EN (expander P0_7) an output, low, as Android's probe leaves it; 10 ms\n");
	if (!pcal_bt_en_bit(PCAL_DAT_OUT, false) || !pcal_bt_en_bit(PCAL_CONFIG, false)) {
		return false;
	}
	sleep_ms(10);
	say("bt: 4: BT_EN high: the chip powered; %u ms\n", BT_POWER_MS);
	powered = true;
	if (!pcal_bt_en_bit(PCAL_DAT_OUT, true)) {
		return false;
	}
	sleep_ms(BT_POWER_MS);
	look_expander("4");
	look_pins("4");
	say("bt: 4: UMSTAT %x (CTS %s), UFSTAT %x\n", rd(uart, UMSTAT), (rd(uart, UMSTAT) & UMSTAT_CTS) ? "asserted" : "not "
	    "asserted", rd(uart, UFSTAT));
	{
		int c, n = 0;

		while ((c = bt_getc()) >= 0 && n < 64) {
			if (n++ == 0) {
				say("bt: 4: the chip sent on its own:");
			}
			say(" %02x", c);
		}
		say(n ? "\n" : "bt: 4: nothing from the chip on its own (as expected)\n");
	}
	return true;
}

/* ---- HCI ----------------------------------------------------------------------------------------------------- */

/*
 * A command, and its Command Complete waited for: true with its status 0 and the return parameters after the status
 * copied out. A Command Status with an error ends it; other events are logged and passed over.
 */
static bool
hci(uint16_t opcode, const uint8_t *param, uint8_t plen, uint8_t *ret, uint8_t *retlen, uint32_t timeout_ms, bool quiet)
{
	uint8_t pkt[4 + 255];
	size_t n = h4_command(pkt, sizeof(pkt), opcode, param, plen);
	uint64_t until;
	uint32_t junk_before = rx.dropped_bytes;

	h4_rx_reset(&rx);
	if (n == 0 || !bt_write(pkt, n, timeout_ms)) {
		return false;
	}
	until = deadline_ms(timeout_ms);
	while (mach_absolute_time() < until) {
		int c = bt_getc();
		uint16_t op;
		uint8_t status, rl;
		const uint8_t *r;

		if (c < 0) {
			sleep_ms(1);
			continue;
		}
		if (h4_rx_byte(&rx, (uint8_t)c) != H4_RX_EVENT) {
			continue;
		}
		if (hci_command_complete(rx.event, &op, &status, &r, &rl) && op == opcode) {
			if (rx.dropped_bytes != junk_before) {
				say("bt: %u bytes before %04x's answer were not a packet\n", rx.dropped_bytes - junk_before, opcode);
			}
			if (status != 0) {
				say("bt: command %04x: status %02x\n", opcode, status);
				return false;
			}
			if (ret != NULL) {
				memcpy(ret, r, rl);
				*retlen = rl;
			}
			return true;
		}
		if (hci_command_status(rx.event, &op, &status) && op == opcode && status != 0) {
			say("bt: command %04x refused: status %02x\n", opcode, status);
			return false;
		}
		if (accept_status && hci_command_status(rx.event, &op, &status) && op == opcode) {
			return true;
		}
		if (scanning) {
			scan_event(rx.event);
			continue;
		}
		if (!quiet) {
			say("bt: while waiting for %04x: event %02x, %u bytes, passed over\n", opcode, rx.event[0], rx.event[1]);
		}
	}
	say("bt: command %04x: no answer in %u ms (UFSTAT %x UMSTAT %x, %u bytes not a packet, %u receive errors)\n", opcode,
	    timeout_ms, rd(uart, UFSTAT), rd(uart, UMSTAT), rx.dropped_bytes - junk_before, rx_errors);
	return false;
}

static bool
identify(const char *when, struct hci_local_version *v)
{
	uint8_t ret[255], rl = 0;
	char name[64];

	say("bt: %s: HCI_Reset\n", when);
	if (!hci(HCI_RESET, NULL, 0, NULL, NULL, CMD_TIMEOUT_MS, false)) {
		return false;
	}
	say("bt: %s: Read Local Version\n", when);
	if (!hci(HCI_READ_LOCAL_VERSION, NULL, 0, ret, &rl, CMD_TIMEOUT_MS, false) || !hci_local_version(ret, rl, v)) {
		return false;
	}
	say("bt: %s: HCI version %u revision %04x, LMP version %u subversion %04x, manufacturer %04x%s\n", when, v->hci_ver,
	    v->hci_rev, v->lmp_ver, v->lmp_subver, v->manufacturer,
	    v->manufacturer == BCM_MANUFACTURER ? " (Broadcom)" : " (not Broadcom's 000f)");
	if (hci(HCI_READ_LOCAL_NAME, NULL, 0, ret, &rl, CMD_TIMEOUT_MS, false)) {
		hci_local_name(ret, rl, name, sizeof(name));
		say("bt: %s: local name \"%s\"\n", when, name);
	}
	return true;
}

/*
 * Starbite port: the patch from the iOS root filesystem (the header's note). A file of at most 1 MB, a regular one,
 * read whole into memory and kept (bt=7 may patch the chip a second time). 0 or an errno.
 */
#define HCD_DIR                 "/private/var/hakios/bt/"
#define HCD_MAX                 (1u << 20)

static int
root_file(const char *path, uint64_t max, uint8_t **data, uint64_t *size)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	off_t len = 0;
	int e, resid = 0;
	uint8_t *buf;

	if (rootvnode == NULLVP) {
		return EAGAIN;
	}
	if ((e = vnode_open(path, FREAD, 0, 0, &vp, ctx)) != 0) {
		return e;
	}
	if (vnode_vtype(vp) != VREG) {
		e = EFTYPE;
	} else if ((e = vnode_size(vp, &len, ctx)) == 0 && (len <= 0 || (uint64_t)len > max)) {
		e = EFBIG;
	}
	if (e == 0) {
		buf = kalloc_data((vm_size_t)len, Z_WAITOK | Z_ZERO);
		if (buf == NULL) {
			e = ENOMEM;
		} else if ((e = vn_rdwr(UIO_READ, vp, (caddr_t)buf, (int)len, 0, UIO_SYSSPACE, 0, vfs_context_ucred(ctx),
		    &resid, current_proc())) != 0 || resid != 0) {
			kfree_data(buf, (vm_size_t)len);
			e = e != 0 ? e : EIO;
		} else {
			*data = buf;
			*size = (uint64_t)len;
		}
	}
	(void)vnode_close(vp, FREAD, ctx);
	return e;
}

static uint8_t *hcd_data;
static uint64_t hcd_size;
static char hcd_file[128];

/* the candidates in turn, each second, until one is read or bt_hcd_wait seconds have gone */
static const uint8_t *
module_patch(char *file, size_t room, uint64_t *size)
{
	char arg[96] = "", module[16] = "", paths[2][128];
	unsigned npaths = 0;
	uint32_t wait_s = 180;
	bool guessed = false;
	int last[2] = { 0, 0 };

	if (hcd_data != NULL) {
		strlcpy(file, hcd_file, room);
		*size = hcd_size;
		return hcd_data;
	}
	PE_parse_boot_argn("bt_hcd_wait", &wait_s, sizeof(wait_s));
	if (PE_parse_boot_arg_str("bt_hcd", arg, sizeof(arg)) && arg[0] != 0) {
		snprintf(paths[npaths++], sizeof(paths[0]), "%s%s", arg[0] == '/' ? "" : HCD_DIR, arg);
	} else if (PE_parse_boot_arg_str("bt_module", module, sizeof(module)) && module[0] != 0) {
		snprintf(paths[npaths++], sizeof(paths[0]), HCD_DIR "bcm4361B2_%s.hcd", module);
	} else {
		guessed = true;
		snprintf(paths[npaths++], sizeof(paths[0]), HCD_DIR "bcm4361B2_murata.hcd");
		snprintf(paths[npaths++], sizeof(paths[0]), HCD_DIR "bcm4361B2_semco.hcd");
	}
	say("bt: 6: the patch from the root filesystem: %s%s%s (waiting up to %u s for it)\n", paths[0],
	    npaths > 1 ? ", else " : "", npaths > 1 ? paths[1] : "", wait_s);
	for (uint32_t waited = 0; ; waited++) {
		for (unsigned k = 0; k < npaths; k++) {
			uint8_t *d = NULL;
			uint64_t s = 0;

			last[k] = root_file(paths[k], HCD_MAX, &d, &s);
			if (last[k] == 0) {
				hcd_data = d;
				hcd_size = s;
				strlcpy(hcd_file, paths[k], sizeof(hcd_file));
				strlcpy(file, paths[k], room);
				*size = s;
				say("bt: 6: the patch is %s%s\n", paths[k], guessed ? ": A GUESS of the module (bt_module=murata or "
				    "semco says which)" : "");
				return hcd_data;
			}
		}
		if (waited >= wait_s) {
			break;
		}
		if (waited % 30 == 0) {
			say("bt: 6: %s (%s, errno %d)%s: waiting\n", paths[0], rootvnode == NULLVP ? "no root filesystem yet" :
			    "not read", last[0], npaths > 1 ? " and the other" : "");
		}
		sleep_ms(1000);
	}
	snprintf(file, room, "%s", paths[0]);
	say("bt: 6: %s not read (errno %d%s) after %u s\n", paths[0], last[0], rootvnode == NULLVP ? ", no root filesystem" :
	    "", wait_s);
	return NULL;
}

static bool
patch(void)
{
	char file[128] = "";
	const uint8_t *data = NULL;
	uint64_t size = 0;
	struct bcm_hcd_info info;
	struct bcm_hcd_record rec;
	size_t off = 0;
	uint32_t sent = 0;
	int r;

	data = module_patch(file, sizeof(file), &size);
	if (data == NULL) {
		say("bt: 6: SKIPPED: no patch (put the phone's own bcm4361B2_<module>.hcd, from its VENDOR partition, in "
		    HCD_DIR ", or name one with bt_hcd=<path>)\n");
		return false;
	}
	r = bcm_hcd_check(data, (size_t)size, &info);
	if (r != BCM_HCD_OK) {
		say("bt: 6: %s (%llu bytes) is not a patch to send: %s (after %u records)\n", file, size, bcm_hcd_error(r),
		    info.records);
		return false;
	}
	say("bt: 6: %s: %llu bytes, %u records: %u Write_RAM (%u bytes), %u other, Launch_RAM at %08x; \"%s\"\n", file,
	    size, info.records, info.writes, info.write_bytes, info.others, info.launch_address,
	    info.name[0] ? info.name : "(no description)");
	say("bt: 6: Download Minidriver (fc2e), then %u ms\n", MINIDRV_MS);
	if (!hci(BCM_DOWNLOAD_MINIDRIVER, NULL, 0, NULL, NULL, CMD_TIMEOUT_MS, false)) {
		return false;
	}
	sleep_ms(MINIDRV_MS);
	say("bt: 6: the records, one by one, each answered before the next\n");
	while (bcm_hcd_next(data, (size_t)size, &off, &rec) == 1) {
		bool last = rec.opcode == BCM_LAUNCH_RAM;

		if (last) {
			say("bt: 6: %u records sent; Launch_RAM\n", sent);
		}
		if (!hci(rec.opcode, rec.param, rec.plen, NULL, NULL, last ? 1000 : CMD_TIMEOUT_MS, true)) {
			if (!last) {
				say("bt: 6: record %u of %u (%04x) failed\n", sent + 1, info.records, rec.opcode);
				return false;
			}
			say("bt: 6: Launch_RAM unanswered: the chip may have restarted into the patch without saying so; going on\n");
		}
		sent++;
		if (!last && sent % 100 == 0) {
			say("bt: 6: %u of %u\n", sent, info.records);
		}
	}
	say("bt: 6: %u ms for the patch to start\n", LAUNCH_MS);
	sleep_ms(LAUNCH_MS);
	fifo_reset();
	return true;
}

/* ---- bt=7: the link at speed, the chip kept, and the USB console's "bt" ---------------------------------------- */

#define BCM_UPDATE_BAUD         0xFC18  /* hci_bcm.c:117: two zero bytes, the rate le32 */
#define BCM_UART_CLOCK          0xFC45  /* hci_bcm.c:106: 1 = 48 MHz, needed above 3 Mbaud */
#define HCI_SET_EVENT_MASK      0x0C01
#define HCI_WRITE_INQUIRY_MODE  0x0C45
#define HCI_READ_BD_ADDR        0x1009
#define HCI_INQUIRY             0x0401
#define HCI_LE_SET_SCAN_PARAMS  0x200B
#define HCI_LE_SET_SCAN_ENABLE  0x200C
#define UTRSTAT_TX_EMPTY        (1u << 2)
#define SCAN_MAX                48

static uint32_t link_baud = BAUD;
static struct hci_local_version running;
static uint8_t own_addr[6];
static volatile bool resident;          /* the chip powered, patched and answering: the console may use it */
static volatile SInt32 busy;            /* one user of the UART at a time */
static bool inquiry_done;

struct seen {
	uint8_t  addr[6];               /* as HCI sends it, little-endian */
	uint8_t  le;                    /* 0 BR/EDR, 1 LE */
	uint8_t  addr_type;
	int8_t   rssi;
	uint32_t cod;
	uint16_t hits;
	char     name[33];
};
static struct seen seen[SCAN_MAX];
static unsigned nseen, nreports;

/* the UART's rate, once what it is sending has gone */
static bool
uart_set_baud(uint32_t baud)
{
	struct exynos_uart_div d;
	uint64_t until = deadline_ms(50);

	if (!exynos_uart_divisor(uart_mhz * 1000000u, baud, &d)) {
		say("bt: no divisor for %u baud from %u MHz\n", baud, uart_mhz);
		return false;
	}
	while (!(rd(uart, UTRSTAT) & UTRSTAT_TX_EMPTY) && mach_absolute_time() < until) {
	}
	wr(uart, UBRDIV, d.ubrdiv);
	wr(uart, UFRACVAL, d.fracval);
	say("bt: the UART at %u baud: UBRDIV %u UFRACVAL %u (%u from %u MHz)\n", baud, d.ubrdiv, d.fracval, d.actual, uart_mhz);
	return true;
}

/* hci_bcm.c's bcm_set_baudrate(): the clock at 48 MHz above 3 Mbaud, 0xFC18 answered at the old rate, then ours */
static bool
raise_baud(uint32_t baud)
{
	uint8_t p[6] = { 0, 0, (uint8_t)baud, (uint8_t)(baud >> 8), (uint8_t)(baud >> 16), (uint8_t)(baud >> 24) };
	uint8_t clock = 1, ret[255], rl = 0;
	struct hci_local_version v;

	if (baud > 3000000) {
		say("bt: 7: Write_UART_Clock_Setting (fc45) 48 MHz\n");
		if (!hci(BCM_UART_CLOCK, &clock, 1, NULL, NULL, CMD_TIMEOUT_MS, false)) {
			say("bt: 7: fc45 not taken (the BCM4347B2 of this module has no such command): going on without it\n");
		}
	}
	say("bt: 7: Update_UART_Baud_Rate (fc18) %u, then the UART to it\n", baud);
	if (!hci(BCM_UPDATE_BAUD, p, sizeof(p), NULL, NULL, CMD_TIMEOUT_MS, false)) {
		return false;
	}
	if (!uart_set_baud(baud)) {
		return false;
	}
	sleep_ms(10);
	fifo_reset();
	link_baud = baud;
	if (!hci(HCI_READ_LOCAL_VERSION, NULL, 0, ret, &rl, CMD_TIMEOUT_MS, false) || !hci_local_version(ret, rl, &v)) {
		say("bt: 7: no answer at %u baud (%u receive errors)\n", baud, rx_errors);
		return false;
	}
	say("bt: 7: at %u baud, flow control on: Read Local Version answers (LMP subversion %04x, HCI revision %04x)\n",
	    baud, v.lmp_subver, v.hci_rev);
	return true;
}

static void
addr_text(const uint8_t *a, char *out, size_t cap)
{
	snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
}

/* a name from EIR or advertising data: 0x09 complete name, else 0x08 shortened */
static void
ad_name(const uint8_t *ad, unsigned len, char *name, size_t cap)
{
	bool complete = false;

	for (unsigned i = 0; i + 1 < len && ad[i] != 0; i += 1u + ad[i]) {
		unsigned l = ad[i], type = ad[i + 1], k = 0;

		if (i + 1 + l > len || l < 2 || (type != 0x08 && type != 0x09) || (complete && type == 0x08)) {
			continue;
		}
		for (unsigned j = 0; j < l - 1 && k + 1 < cap; j++) {
			uint8_t c = ad[i + 2 + j];

			name[k++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
		}
		name[k] = 0;
		complete = type == 0x09;
	}
}

static struct seen *
seen_at(const uint8_t *addr, uint8_t le)
{
	for (unsigned i = 0; i < nseen; i++) {
		if (seen[i].le == le && memcmp(seen[i].addr, addr, 6) == 0) {
			return &seen[i];
		}
	}
	if (nseen == SCAN_MAX) {
		return NULL;
	}
	memset(&seen[nseen], 0, sizeof(seen[0]));
	memcpy(seen[nseen].addr, addr, 6);
	seen[nseen].le = le;
	seen[nseen].rssi = 127;
	return &seen[nseen++];
}

static void
heard(const uint8_t *addr, uint8_t le, uint8_t addr_type, int rssi, uint32_t cod, const uint8_t *ad, unsigned adlen)
{
	struct seen *s = seen_at(addr, le);

	nreports++;
	if (s == NULL) {
		return;
	}
	s->hits++;
	s->addr_type = addr_type;
	if (rssi != 127) {
		s->rssi = (int8_t)rssi;
	}
	if (cod != 0) {
		s->cod = cod;
	}
	if (ad != NULL && adlen != 0) {
		char name[sizeof(s->name)] = "";

		ad_name(ad, adlen, name, sizeof(name));
		if (name[0] != 0) {
			strlcpy(s->name, name, sizeof(s->name));
		}
	}
}

/* Inquiry Result (02), with RSSI (22), Extended (2f), Inquiry Complete (01), LE Advertising Report (3e/02) */
static void
scan_event(const uint8_t *event)
{
	unsigned len = event[1];
	const uint8_t *p = event + 2;

	switch (event[0]) {
	case 0x01:
		inquiry_done = true;
		break;
	case 0x02:
		for (unsigned i = 0; len >= 1 && i < p[0] && 1 + 14 * (i + 1) <= len; i++) {
			const uint8_t *r = p + 1 + 14 * i;

			heard(r, 0, 0, 127, (uint32_t)r[9] | (uint32_t)r[10] << 8 | (uint32_t)r[11] << 16, NULL, 0);
		}
		break;
	case 0x22:
		for (unsigned i = 0; len >= 1 && i < p[0] && 1 + 14 * (i + 1) <= len; i++) {
			const uint8_t *r = p + 1 + 14 * i;

			heard(r, 0, 0, (int8_t)r[13], (uint32_t)r[8] | (uint32_t)r[9] << 8 | (uint32_t)r[10] << 16, NULL, 0);
		}
		break;
	case 0x2F:
		if (len >= 15) {
			const uint8_t *r = p + 1;

			heard(r, 0, 0, (int8_t)r[13], (uint32_t)r[8] | (uint32_t)r[9] << 8 | (uint32_t)r[10] << 16, r + 14,
			    len - 15);
		}
		break;
	case 0x3E:
		if (len >= 2 && p[0] == 0x02) {
			unsigned n = p[1], at = 2;

			for (unsigned i = 0; i < n && at + 10 <= len; i++) {
				unsigned dlen = p[at + 8];

				if (at + 10 + dlen > len) {
					break;
				}
				heard(p + at + 2, 1, p[at + 1], (int8_t)p[at + 9 + dlen], 0, p + at + 9, dlen);
				at += 10 + dlen;
			}
		}
		break;
	default:
		break;
	}
}

/* events for a while, each to the scan */
static void
collect(uint32_t ms, volatile bool *until_set)
{
	uint64_t until = deadline_ms(ms);

	while (mach_absolute_time() < until && (until_set == NULL || !*until_set)) {
		int c = bt_getc();

		if (c < 0) {
			sleep_ms(1);
			continue;
		}
		if (h4_rx_byte(&rx, (uint8_t)c) == H4_RX_EVENT) {
			scan_event(rx.event);
		}
	}
}

/* a command whose Command Status says it started (an inquiry) */
static bool
hci_start(uint16_t opcode, const uint8_t *param, uint8_t plen)
{
	bool ok;

	accept_status = true;
	ok = hci(opcode, param, plen, NULL, NULL, CMD_TIMEOUT_MS, true);
	accept_status = false;
	return ok;
}

/*
 * A scan: Set Event Mask with LE Meta (bit 61) on, Write Inquiry Mode 2 (results with RSSI and EIR, so names), a general
 * inquiry (LAP 9e8b33) of 6 x 1.28 s, then an active LE scan of 5 s; every device heard, once, at the end.
 */
static void
bt_scan(void)
{
	static const uint8_t mask[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0x00, 0x20 };
	static const uint8_t mode = 2, inquiry[5] = { 0x33, 0x8b, 0x9e, 6, 0 };
	static const uint8_t le_params[7] = { 1, 0x60, 0x00, 0x30, 0x00, 0, 0 }, le_on[2] = { 1, 1 }, le_off[2] = { 0, 0 };
	unsigned classic = 0;

	nseen = 0;
	nreports = 0;
	inquiry_done = false;
	scanning = true;
	say("bt: scan: Set Event Mask (LE Meta on), Write Inquiry Mode 2; an inquiry of 7.7 s, then an LE scan of 5 s\n");
	if (!hci(HCI_SET_EVENT_MASK, mask, sizeof(mask), NULL, NULL, CMD_TIMEOUT_MS, false) ||
	    !hci(HCI_WRITE_INQUIRY_MODE, &mode, 1, NULL, NULL, CMD_TIMEOUT_MS, false)) {
		say("bt: scan: the chip refused its setup\n");
	}
	if (hci_start(HCI_INQUIRY, inquiry, sizeof(inquiry))) {
		collect(10000, &inquiry_done);
		say("bt: scan: the inquiry %s, %u reports\n", inquiry_done ? "ended (Inquiry Complete)" : "not ended in 10 s",
		    nreports);
	} else {
		say("bt: scan: Inquiry (0401) not started\n");
	}
	classic = nseen;
	if (hci(HCI_LE_SET_SCAN_PARAMS, le_params, sizeof(le_params), NULL, NULL, CMD_TIMEOUT_MS, false) &&
	    hci(HCI_LE_SET_SCAN_ENABLE, le_on, sizeof(le_on), NULL, NULL, CMD_TIMEOUT_MS, false)) {
		collect(5000, NULL);
		(void)hci(HCI_LE_SET_SCAN_ENABLE, le_off, sizeof(le_off), NULL, NULL, CMD_TIMEOUT_MS, true);
	} else {
		say("bt: scan: the LE scan not started\n");
	}
	scanning = false;
	say("bt: scan: %u devices (%u BR/EDR, %u LE), %u reports, %u receive errors\n", nseen, classic, nseen - classic,
	    nreports, rx_errors);
	for (unsigned i = 0; i < nseen; i++) {
		char a[20];

		addr_text(seen[i].addr, a, sizeof(a));
		if (seen[i].le) {
			say("bt:   %s LE %s rssi %d, %u heard \"%s\"\n", a, seen[i].addr_type ? "random" : "public",
			    seen[i].rssi == 127 ? 0 : seen[i].rssi, seen[i].hits, seen[i].name);
		} else {
			say("bt:   %s BR/EDR class %06x rssi %d \"%s\"\n", a, seen[i].cod, seen[i].rssi == 127 ? 0 : seen[i].rssi,
			    seen[i].name);
		}
	}
}

static void
console_scan_thread(__unused void *arg, __unused wait_result_t w)
{
	bt_scan();
	OSCompareAndSwap(1, 0, &busy);
	thread_terminate(current_thread());
}

/* the USB console's "bt" (hakios.dwc3.c): the state, or "bt scan" */
void
hakios_bt_console(const char *arg)
{
	char a[20];
	thread_t thread;

	if (!resident) {
		say("usb: bt: the chip is not up (bt=7 brings it up and keeps it; the log says how far it got)\n");
		return;
	}
	if (arg == NULL || arg[0] == 0) {
		addr_text(own_addr, a, sizeof(a));
		say("usb: bt: up, patched, %u baud with RTS/CTS; LMP subversion %04x HCI revision %04x manufacturer %04x; "
		    "address %s; %u receive errors\n", link_baud, running.lmp_subver, running.hci_rev, running.manufacturer, a,
		    rx_errors);
		return;
	}
	if (strcmp(arg, "scan") != 0) {
		say("usb: bt %s: bt, or bt scan\n", arg);
		return;
	}
	if (!OSCompareAndSwap(0, 1, &busy)) {
		say("usb: bt scan: one is under way\n");
		return;
	}
	if (kernel_thread_start(console_scan_thread, NULL, &thread) != KERN_SUCCESS) {
		OSCompareAndSwap(1, 0, &busy);
		say("usb: bt scan: no thread for it\n");
		return;
	}
	thread_deallocate(thread);
}

/* ---- bt=7: /dev/cu.bluetooth, the chip's HCI for user space ------------------------------------------------------ */

/*
 * What iOS's bluetoothd opens on an iPhone whose controller is on a UART (its strings: "/dev/cu.bluetooth",
 * "/dev/uart.bluetooth", "No valid serial port found (tried cu.bluetooth and uart.bluetooth)"): a serial port, H4 on it.
 * Here a tty of XNU's own (tty.c's line discipline, so termios, select and the rest are the kernel's), whose output goes
 * to UART_BT and whose input is what the chip sends, moved by one thread that polls (1 ms when idle). The chip is up and
 * patched before anyone opens it (bt=7); a speed set with tcsetattr() is set on the UART too, so that a host that sends
 * the chip Broadcom's 0xFC18 itself can follow it. While it is open the console's "bt scan" is refused.
 */
static struct tty *bt_tty;
static volatile bool pump_running;
static volatile SInt32 kctl_owns;       /* 1 while a com.apple.uart.bluetooth client has the UART (below) */
static void kctl_pump(int *in);

static void
bt_tty_start(__unused struct tty *tp)
{
	/* the pump takes t_outq within a millisecond */
}

static int
bt_tty_param(struct tty *tp, struct termios *t)
{
	if (t->c_ospeed != 0 && t->c_ospeed != (speed_t)link_baud && t->c_ospeed >= 9600 && t->c_ospeed <= 4000000) {
		if (uart_set_baud((uint32_t)t->c_ospeed)) {
			link_baud = (uint32_t)t->c_ospeed;
		}
	}
	tp->t_ispeed = tp->t_ospeed = (speed_t)link_baud;
	tp->t_cflag = t->c_cflag;
	return 0;
}

static void
bt_tty_pump(__unused void *arg, __unused wait_result_t w)
{
	static uint8_t buf[256];

	for (;;) {
		struct tty *tp = bt_tty;
		int out = 0, in = 0, c;

		tty_lock(tp);
		if (ISSET(tp->t_state, TS_ISOPEN)) {
			out = q_to_b(&tp->t_outq, buf, sizeof(buf));
		}
		tty_unlock(tp);
		if (out > 0 && !bt_write(buf, (size_t)out, 2000)) {
			say("bt: tty: %d bytes not sent\n", out);
		}
		tty_lock(tp);
		if (out > 0) {
			ttwwakeup(tp);
		}
		/* while the reader keeps up: else the bytes wait in the FIFO and RTS holds the chip */
		while (ISSET(tp->t_state, TS_ISOPEN) && tp->t_rawq.c_cc < TTYHOG / 2 && in < 1024 && (c = bt_getc()) >= 0) {
			(*linesw[tp->t_line].l_rint)(c, tp);
			in++;
		}
		tty_unlock(tp);
		if (kctl_owns) {
			kctl_pump(&in);
		}
		if (out == 0 && in == 0) {
			sleep_ms(1);
		}
	}
}

static int
bt_cu_open(dev_t dev, int flag, __unused int devtype, __unused proc_t p)
{
	struct tty *tp = bt_tty;
	int error;

	if (!resident) {
		return ENXIO;
	}
	tty_lock(tp);
	if (!ISSET(tp->t_state, TS_ISOPEN)) {
		if (OSCompareAndSwap(1, 0, &kctl_owns)) {
			/* BlueTool's session (below) gives way to bluetoothd: busy stays taken, now by the tty */
			say("bt: tty: opened by pid %d while com.apple.uart.bluetooth had the UART: the tty has it now\n",
			    proc_pid(current_proc()));
		} else if (!OSCompareAndSwap(0, 1, &busy)) {
			tty_unlock(tp);
			return EBUSY;
		}
		termioschars(&tp->t_termios);
		tp->t_iflag = 0;
		tp->t_oflag = 0;
		tp->t_lflag = 0;
		tp->t_cflag = CS8 | CREAD | CLOCAL | CRTSCTS;
		tp->t_ispeed = tp->t_ospeed = (speed_t)link_baud;
		ttsetwater(tp);
		tp->t_dev = dev;
		tp->t_oproc = bt_tty_start;
		tp->t_param = bt_tty_param;
		fifo_reset();
	}
	SET(tp->t_state, TS_CARR_ON | TS_CONNECTED);
	error = (*linesw[tp->t_line].l_open)(dev, tp);
	if (error == 0) {
		say("bt: tty: opened (flags %x) by pid %d\n", flag, proc_pid(current_proc()));
	}
	tty_unlock(tp);
	return error;
}

static int
bt_cu_close(__unused dev_t dev, int flag, __unused int mode, __unused proc_t p)
{
	struct tty *tp = bt_tty;

	tty_lock(tp);
	(*linesw[tp->t_line].l_close)(tp, flag);
	(void)ttyclose(tp);
	tty_unlock(tp);
	OSCompareAndSwap(1, 0, &busy);
	say("bt: tty: closed\n");
	return 0;
}

static int
bt_cu_read(__unused dev_t dev, struct uio *uio, int flag)
{
	struct tty *tp = bt_tty;
	int error;

	tty_lock(tp);
	error = (*linesw[tp->t_line].l_read)(tp, uio, flag);
	tty_unlock(tp);
	return error;
}

static int
bt_cu_write(__unused dev_t dev, struct uio *uio, int flag)
{
	struct tty *tp = bt_tty;
	int error;

	tty_lock(tp);
	error = (*linesw[tp->t_line].l_write)(tp, uio, flag);
	tty_unlock(tp);
	return error;
}

static int
bt_cu_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, int flag, proc_t p)
{
	struct tty *tp = bt_tty;
	int error;

	tty_lock(tp);
	error = (*linesw[tp->t_line].l_ioctl)(tp, cmd, data, flag, p);
	if (error == ENOTTY) {
		error = ttioctl_locked(tp, cmd, data, flag, p);
	}
	if (error == ENOTTY && (cmd == TIOCMSET || cmd == TIOCMBIS || cmd == TIOCMBIC || cmd == TIOCSBRK || cmd == TIOCCBRK)) {
		error = 0;      /* the modem lines are the UART's own (RTS/CTS by hardware) */
	}
	if (error == ENOTTY && cmd == TIOCMGET) {
		*(int *)data = TIOCM_LE | TIOCM_DTR | TIOCM_RTS | TIOCM_CAR | TIOCM_DSR |
		    ((rd(uart, UMSTAT) & UMSTAT_CTS) ? TIOCM_CTS : 0);
		error = 0;
	}
	tty_unlock(tp);
	return error;
}

static int
bt_cu_select(__unused dev_t dev, int rw, void *wql, proc_t p)
{
	return ttyselect(bt_tty, rw, wql, p);
}

static int
bt_cu_stop(__unused struct tty *tp, __unused int rw)
{
	return 0;
}

static const struct cdevsw bt_cdevsw = {
	.d_open = bt_cu_open, .d_close = bt_cu_close, .d_read = bt_cu_read, .d_write = bt_cu_write,
	.d_ioctl = bt_cu_ioctl, .d_stop = bt_cu_stop, .d_reset = eno_reset, .d_ttys = NULL, .d_select = bt_cu_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = D_TTY
};

/*
 * com.apple.uart.bluetooth: the UART as iOS's BlueTool opens it. BlueTool 16.5 ("device -D", or "device -d <name>")
 * does not open a /dev node: it makes a PF_SYSTEM stream socket, asks CTLIOCGINFO for the kernel control of that name
 * (default com.apple.uart.bluetooth, or $BT_UART), connects to it, reads and sets termios with get/setsockopt at
 * SYSPROTO_CONTROL (the option numbers are TIOCGETA and TIOCSETA), writes H4 with write() and reads it with read()
 * until it has as many bytes as it asked for (a control registered for datagrams refuses its connect: EPROTOTYPE). With
 * no such control, every BlueTool command that needs the chip says "Device not open yet" and bluetoothd's boot of the
 * chip through BlueTool fails (seen on the phone, 3 November). bluetoothd's own transport
 * (BT_HCI_TRANSPORT=H4:POSIX:<name>,<rate>, its "POSIX" driver) is the same kind of socket, so it opens this control
 * too. So the same UART, when neither the tty nor the console has it: what is sent goes out as it comes; what the chip
 * sends is queued to the socket as it is read; the rate is the kernel's (the chip was raised to it at bt=7), so a rate
 * set through termios is answered and not followed. One client at a time, the newest: BlueTool keeps its socket open
 * after its scripts, so a new connection (bluetoothd's) takes the UART from it, as does an open of /dev/cu.bluetooth;
 * the client cut off finds its writes failing with ENXIO.
 */
/*
 * bt_trace=N: what goes over com.apple.uart.bluetooth, packet by packet, as the console can take it (N lines a second;
 * the rest counted). One H4 reader for each direction, fed the bytes as they pass, in whatever pieces they come: the
 * type byte, the header (command: opcode and length; ACL: handle and flags, length; SCO: handle, length; event: code
 * and length), then the payload, of which the first bytes are kept for the line. A type byte that is none of the four
 * is said once and the reader looks for the next one (the stream has lost its framing: that is worth seeing).
 */
static uint32_t trace_per_s;
/*
 * Also sysctl kern.hakios.bt_trace (the console's "sysctl kern.hakios.bt_trace=60"): the boot-arg is the last of a long
 * line and the loader cuts what does not fit in the kernel's 607 characters from its end, so it can be lost there.
 */
SYSCTL_DECL(_kern_hakios);
SYSCTL_UINT(_kern_hakios, OID_AUTO, bt_trace, CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED, &trace_per_s, 0,
    "bt=7: H4 packets on com.apple.uart.bluetooth to the console, at most this many lines a second (0: none)");
struct h4_trace {
	const char *dir;                /* "H>" host to chip, "<C" chip to host */
	uint8_t type;                   /* 0: waiting for a type byte */
	uint8_t hdr[4], hlen, hneed;
	uint32_t plen, pgot;
	uint8_t first[16];
	uint32_t junk;
};
static struct h4_trace trace_out = { .dir = "H>" }, trace_in = { .dir = "<C" };
static uint64_t trace_window;           /* mach time the current second began */
static uint32_t trace_lines, trace_skipped;

static void
trace_line(struct h4_trace *t)
{
	char line[200];
	uint64_t now = mach_absolute_time(), ns;
	uint32_t keep = t->pgot < sizeof(t->first) ? t->pgot : (uint32_t)sizeof(t->first);
	int at;

	absolutetime_to_nanoseconds(now - trace_window, &ns);
	if (ns >= NSEC_PER_SEC) {
		trace_window = now;
		trace_lines = 0;
	}
	if (trace_lines >= trace_per_s) {
		trace_skipped++;
		return;
	}
	trace_lines++;
	absolutetime_to_nanoseconds(now, &ns);
	at = snprintf(line, sizeof(line), "bt: %llu.%03llu %s ", ns / NSEC_PER_SEC, (ns / NSEC_PER_MSEC) % 1000, t->dir);
	switch (t->type) {
	case 1:
		at += snprintf(line + at, sizeof(line) - at, "cmd %04x", t->hdr[0] | t->hdr[1] << 8);
		break;
	case 2:
		at += snprintf(line + at, sizeof(line) - at, "acl %03x pb%u", (t->hdr[0] | t->hdr[1] << 8) & 0xfff,
		    (t->hdr[1] >> 4) & 3);
		break;
	case 3:
		at += snprintf(line + at, sizeof(line) - at, "sco %03x", (t->hdr[0] | t->hdr[1] << 8) & 0xfff);
		break;
	default:
		at += snprintf(line + at, sizeof(line) - at, "evt %02x", t->hdr[0]);
		break;
	}
	at += snprintf(line + at, sizeof(line) - at, " len %u:", t->plen);
	for (uint32_t i = 0; i < keep && at < (int)sizeof(line) - 8; i++) {
		at += snprintf(line + at, sizeof(line) - at, " %02x", t->first[i]);
	}
	if (trace_skipped != 0 && at < (int)sizeof(line) - 40) {
		at += snprintf(line + at, sizeof(line) - at, " (%u not shown before)", trace_skipped);
		trace_skipped = 0;
	}
	snprintf(line + at, sizeof(line) - at, "\n");
	(void)say("%s", line);
}

static void
trace_bytes(struct h4_trace *t, const uint8_t *b, size_t n)
{
	if (trace_per_s == 0) {
		return;
	}
	for (size_t i = 0; i < n; i++) {
		uint8_t c = b[i];

		if (t->type == 0) {
			if (c < 1 || c > 4) {
				if (t->junk++ == 0) {
					(void)say("bt: %s %02x is not an H4 packet type: the framing is lost; looking for the next\n", t->dir, c);
				}
				continue;
			}
			if (t->junk != 0) {
				(void)say("bt: %s %u bytes not a packet\n", t->dir, t->junk);
				t->junk = 0;
			}
			t->type = c;
			t->hlen = 0;
			t->hneed = c == 1 ? 3 : c == 2 ? 4 : c == 3 ? 3 : 2;
			t->pgot = 0;
			continue;
		}
		if (t->hlen < t->hneed) {
			t->hdr[t->hlen++] = c;
			if (t->hlen < t->hneed) {
				continue;
			}
			t->plen = t->type == 1 ? t->hdr[2] : t->type == 2 ? (uint32_t)(t->hdr[2] | t->hdr[3] << 8) :
			    t->type == 3 ? t->hdr[2] : t->hdr[1];
		} else {
			if (t->pgot < sizeof(t->first)) {
				t->first[t->pgot] = c;
			}
			t->pgot++;
		}
		if (t->hlen == t->hneed && t->pgot == t->plen) {
			trace_line(t);
			t->type = 0;
		}
	}
}

static kern_ctl_ref kctl_ref;
static volatile u_int32_t kctl_unit;
static uint8_t kctl_rx[256];            /* read from the chip, not yet taken by the socket */
static size_t kctl_rx_len;
static uint32_t kctl_bytes_in, kctl_bytes_out;
static uint32_t kctl_full;               /* times the socket had no room (bt_trace) */

static errno_t
kctl_connect(__unused kern_ctl_ref ref, struct sockaddr_ctl *sac, void **unitinfo)
{
	*unitinfo = NULL;
	if (!resident) {
		return ENXIO;
	}
	if (kctl_owns) {
		/* the newest client has it: BlueTool keeps its socket after its script, and bluetoothd comes next */
		say("bt: com.apple.uart.bluetooth: unit %u gives way to pid %d\n", kctl_unit, proc_pid(current_proc()));
	} else if (!OSCompareAndSwap(0, 1, &busy)) {
		say("bt: com.apple.uart.bluetooth: refused to pid %d: the UART is in use\n", proc_pid(current_proc()));
		return EBUSY;
	}
	kctl_unit = sac->sc_unit;
	kctl_rx_len = 0;
	kctl_bytes_in = kctl_bytes_out = 0;
	trace_out.type = trace_in.type = 0;
	trace_out.junk = trace_in.junk = 0;
	fifo_reset();
	OSCompareAndSwap(0, 1, &kctl_owns);
	say("bt: com.apple.uart.bluetooth: connected (unit %u) by pid %d, the chip at %u baud\n", sac->sc_unit,
	    proc_pid(current_proc()), link_baud);
	return 0;
}

static errno_t
kctl_disconnect(__unused kern_ctl_ref ref, u_int32_t unit, __unused void *unitinfo)
{
	if (unit == kctl_unit && OSCompareAndSwap(1, 0, &kctl_owns)) {
		kctl_unit = 0;
		OSCompareAndSwap(1, 0, &busy);
	}
	say("bt: com.apple.uart.bluetooth: unit %u closed (%u bytes to the chip, %u from it)\n", unit, kctl_bytes_out,
	    kctl_bytes_in);
	return 0;
}

static errno_t
kctl_send(__unused kern_ctl_ref ref, u_int32_t unit, __unused void *unitinfo, mbuf_t m, __unused int flags)
{
	uint8_t buf[256];
	size_t len = mbuf_pkthdr_len(m), off = 0;
	errno_t error = 0;

	if (!kctl_owns || unit != kctl_unit) {
		error = ENXIO;
	}
	while (error == 0 && off < len) {
		size_t n = len - off < sizeof(buf) ? len - off : sizeof(buf);

		if (mbuf_copydata(m, off, n, buf) != 0) {
			error = EINVAL;
		} else if (!bt_write(buf, n, 2000)) {
			error = EIO;
		} else {
			trace_bytes(&trace_out, buf, n);
			off += n;
			kctl_bytes_out += (uint32_t)n;
		}
	}
	mbuf_freem(m);
	return error;
}

/* the termios BlueTool reads and sets: 8 bits, RTS/CTS, the kernel's rate */
static void
kctl_termios(struct termios *t)
{
	bzero(t, sizeof(*t));
	t->c_cflag = CS8 | CREAD | CLOCAL | CRTSCTS;
	t->c_ispeed = t->c_ospeed = (speed_t)link_baud;
}

static errno_t
kctl_getopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data,
    size_t *len)
{
	if ((u_long)(u_int)opt != (u_long)(u_int)TIOCGETA) {
		return ENOPROTOOPT;
	}
	if (data != NULL) {
		struct termios t;

		if (*len < sizeof(t)) {
			return EINVAL;
		}
		kctl_termios(&t);
		memcpy(data, &t, sizeof(t));
	}
	*len = sizeof(struct termios);
	return 0;
}

static errno_t
kctl_setopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data,
    size_t len)
{
	u_int o = (u_int)opt;

	if (o != (u_int)TIOCSETA && o != (u_int)TIOCSETAW && o != (u_int)TIOCSETAF) {
		return ENOPROTOOPT;
	}
	if (data != NULL && len >= sizeof(struct termios)) {
		const struct termios *t = data;

		if (t->c_ospeed != 0 && t->c_ospeed != (speed_t)link_baud) {
			say("bt: com.apple.uart.bluetooth: %lu baud asked; the chip stays at %u\n", (unsigned long)t->c_ospeed,
			    link_baud);
		}
	}
	return 0;
}

/* from the pump: what the chip sent, to the socket, while it has room */
static void
kctl_pump(int *in)
{
	u_int32_t unit = kctl_unit;
	int c;

	while (kctl_owns && *in < 1024) {
		while (kctl_rx_len < sizeof(kctl_rx) && (c = bt_getc()) >= 0) {
			kctl_rx[kctl_rx_len++] = (uint8_t)c;
		}
		if (kctl_rx_len == 0) {
			return;
		}
		if (ctl_enqueuedata(kctl_ref, unit, kctl_rx, kctl_rx_len, 0) != 0) {
			if (trace_per_s != 0 && kctl_full++ % 1000 == 0) {
				(void)say("bt: com.apple.uart.bluetooth: the reader is behind (%u times): %zu bytes wait\n", kctl_full, kctl_rx_len);
			}
			return;                         /* ENOBUFS: the reader is behind; RTS holds the chip meanwhile */
		}
		trace_bytes(&trace_in, kctl_rx, kctl_rx_len);
		kctl_bytes_in += (uint32_t)kctl_rx_len;
		*in += (int)kctl_rx_len;
		kctl_rx_len = 0;
	}
}

static void
kctl_publish(void)
{
	struct kern_ctl_reg reg;
	errno_t e;

	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, "com.apple.uart.bluetooth", sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;      /* a stream, as BlueTool's socket; not privileged: it runs as mobile */
	reg.ctl_recvsize = 64 * 1024;
	reg.ctl_connect = kctl_connect;
	reg.ctl_disconnect = kctl_disconnect;
	reg.ctl_send = kctl_send;
	reg.ctl_getopt = kctl_getopt;
	reg.ctl_setopt = kctl_setopt;
	PE_parse_boot_argn("bt_trace", &trace_per_s, sizeof(trace_per_s));
	e = ctl_register(&reg, &kctl_ref);
	say("bt: com.apple.uart.bluetooth: %s (%d)\n", e == 0 ? "the kernel control for BlueTool" : "not registered", e);
}

/* the nodes, once: cu.bluetooth, uart.bluetooth and tty.bluetooth, one tty */
static void
bt_tty_publish(void)
{
	static const char *const names[] = { "cu.bluetooth", "uart.bluetooth", "tty.bluetooth" };
	thread_t thread;
	int major;

	if (bt_tty != NULL) {
		return;
	}
	bt_tty = ttymalloc();
	major = cdevsw_add(-1, &bt_cdevsw);
	if (bt_tty == NULL || major < 0) {
		say("bt: tty: no tty or no device number: no /dev/cu.bluetooth\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "%s", names[i]) == NULL) {
			say("bt: tty: /dev/%s not made\n", names[i]);
		}
	}
	kctl_publish();
	if (kernel_thread_start(bt_tty_pump, NULL, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "bt-tty");
		thread_deallocate(thread);
		pump_running = true;
	}
	say("bt: tty: /dev/cu.bluetooth, /dev/uart.bluetooth and /dev/tty.bluetooth: the chip's H4 at %u baud, for user space\n",
	    link_baud);
}

/* "8C:83:E1:02:EE:E9" (a line of text, upper or lower case) into HCI's order (the last byte first), or false */
static bool
addr_parse(const char *t, size_t n, uint8_t out[6])
{
	unsigned at = 0;

	for (int i = 5; i >= 0; i--) {
		uint8_t v = 0;

		for (int k = 0; k < 2; k++, at++) {
			char c = at < n ? t[at] : 0;

			v = (uint8_t)(v << 4);
			if (c >= '0' && c <= '9') {
				v |= (uint8_t)(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				v |= (uint8_t)(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				v |= (uint8_t)(c - 'A' + 10);
			} else {
				return false;
			}
		}
		out[i] = v;
		if (i > 0 && (at >= n || t[at++] != ':')) {
			return false;
		}
	}
	return (out[0] | out[1] | out[2] | out[3] | out[4] | out[5]) != 0;
}

/*
 * The phone's own address for the chip, as Samsung's Android gives it: the text in /efs/bluetooth/bt_addr (the EFS
 * partition, read here and never written: drivers/storage/hakios.ext4.c, the phone's storage up as for the patch),
 * sent with Broadcom's Write_BD_ADDR (0xFC01, the address in HCI's order: btbcm.c:93 btbcm_set_bdaddr()). Without it the
 * chip answers with Broadcom's default, 43:61:b2:00:1f:ac, which every S9 then has: two phones near each other, or a
 * headset paired with both, take one for the other. bt_addr=<a> gives one; bt_addr=0 keeps the default.
 */
#define HCI_BCM_WRITE_BD_ADDR   0xFC01

static void
set_own_address(void)
{
	char arg[24] = "", a[20];
	uint8_t want[6], ret[255], rl = 0;
	const char *from;

	if (PE_parse_boot_arg_str("bt_addr", arg, sizeof(arg)) && arg[0] != 0) {
		if (strcmp(arg, "0") == 0) {
			say("bt: 7: bt_addr=0: the chip keeps its default address\n");
			return;
		}
		if (!addr_parse(arg, strlen(arg), want)) {
			say("bt: 7: bt_addr=%s is not an address (xx:xx:xx:xx:xx:xx): the chip keeps its own\n", arg);
			return;
		}
		from = "bt_addr=";
	} else {
		/* Starbite port: no EFS reader in this tree */
		say("bt: 7: no bt_addr=<a> (the phone's EFS:/bluetooth/bt_addr is not read on this kernel): the chip keeps "
		    "its default\n");
		return;
	}
	addr_text(want, a, sizeof(a));
	if (!hci(HCI_BCM_WRITE_BD_ADDR, want, 6, ret, &rl, CMD_TIMEOUT_MS, false)) {
		say("bt: 7: Write_BD_ADDR (0xFC01) %s from %s: not taken; the chip keeps its default\n", a, from);
		return;
	}
	say("bt: 7: Write_BD_ADDR (0xFC01) %s from %s\n", a, from);
}

/* bt=7, after the patch: the rate raised, the address read, the chip kept for the console */
static bool
stage7(void)
{
	uint32_t baud = 4000000;
	uint8_t ret[255], rl = 0;
	char a[20];

	PE_parse_boot_argn("bt_baud", &baud, sizeof(baud));
	if (baud != BAUD && !raise_baud(baud)) {
		struct hci_local_version v;

		/* the chip may be at the new rate and we not: start it again, patch it again, and stay at 115200 */
		say("bt: 7: %u baud did not work: the chip off and on again, patched again, kept at %u\n", baud, BAUD);
		power_off("the rate did not take");
		sleep_ms(200);
		(void)uart_set_baud(BAUD);
		link_baud = BAUD;
		fifo_reset();
		if (!power_on() || !identify("7", &v) || !patch() || !identify("7", &running)) {
			return false;
		}
	}
	set_own_address();
	if (hci(HCI_READ_BD_ADDR, NULL, 0, ret, &rl, CMD_TIMEOUT_MS, false) && rl >= 6) {
		memcpy(own_addr, ret, 6);
	}
	addr_text(own_addr, a, sizeof(a));
	say("bt: 7: the chip's address %s; kept powered for the USB console (\"bt\", \"bt scan\")\n", a);
	if (PE_parse_boot_argn("bt_scan", &baud, sizeof(baud)) && baud != 0 && OSCompareAndSwap(0, 1, &busy)) {
		bt_scan();
		OSCompareAndSwap(1, 0, &busy);
	}
	resident = true;
	if (!PE_parse_boot_argn("bt_tty", &baud, sizeof(baud)) || baud != 0) {
		bt_tty_publish();
		HakiosBTNodeStart(own_addr, link_baud);   /* hakios.bt_node.cpp: the "bluetooth" service bluetoothd looks for */
	}
	return true;
}

/* ---- the steps ----------------------------------------------------------------------------------------------- */

static void
bt_thread(void *arg, wait_result_t wr_unused)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg, keep = 0;
	struct hci_local_version rom, patched;

	(void)wr_unused;
	PE_parse_boot_argn("bt_keep", &keep, sizeof(keep));
	PE_parse_boot_argn("bt_uart_mhz", &uart_mhz, sizeof(uart_mhz));
	if (uart_mhz < 26 || uart_mhz > 400) {
		uart_mhz = 200;
	}
	uart = ml_io_map(UART_PA, 0x1000);
	cmu = ml_io_map(CMU_PERIC1_PA, CMU_PERIC1_SIZE);
	alive = ml_io_map(GPIO_ALIVE_PA, 0x1000);
	peric0 = ml_io_map(GPIO_PERIC0_PA, 0x1000);
	peric1 = ml_io_map(GPIO_PERIC1_PA, 0x1000);
	if (!uart || !cmu || !alive || !peric0 || !peric1 || !hakios_hsi2c_map(&bus, say)) {
		say("bt: a block could not be mapped: off\n");
		goto out;
	}
	say("bt: bt=%u: the BCM4361's Bluetooth on UART_BT (0x10840000), BT_EN on the PCAL6524 at 0x22 of hsi2c@10930000\n",
	    steps);
	say("bt: 1: a look, reading only\n");
	look_clocks();
	look_pins("1");
	if (uart_clocks_open()) {
		look_uart("1");
	} else {
		say("bt: 1: a gate of UART_BT is forced off: its registers are not read\n");
	}
	if (steps < 2) {
		goto out;
	}
	if (!expander_up() || steps < 3) {
		goto out;
	}
	if (!bt_uart_setup() || !bt_uart_loopback()) {
		goto out;
	}
	say("bt: 3: flow control on (RTS/CTS, as Android's)\n");
	wr(uart, UMCON, UMCON_AFC);
	if (steps < 4) {
		goto out;
	}
	if (!power_on() || steps < 5) {
		goto done;
	}
	if (!identify("5", &rom) || steps < 6) {
		goto done;
	}
	if (!patch() || !identify("6", &patched)) {
		goto done;
	}
	say("bt: 6: before the patch %u.%u.%u build %u, after it %u.%u.%u build %u (as btbcm.c:497-499 prints them: the LMP "
	    "subversion's three fields and the HCI revision's low 12 bits)%s\n", rom.lmp_subver >> 13, (rom.lmp_subver >> 8) & 0x1f,
	    rom.lmp_subver & 0xff, rom.hci_rev & 0xfff, patched.lmp_subver >> 13, (patched.lmp_subver >> 8) & 0x1f,
	    patched.lmp_subver & 0xff, patched.hci_rev & 0xfff, rom.hci_rev == patched.hci_rev && rom.lmp_subver ==
	    patched.lmp_subver ? ": nothing changed, so the patch may not have taken" : "");
	running = patched;
	if (steps >= 7 && stage7()) {
		goto out;
	}
done:
	if (keep == 1 && powered) {
		say("bt: bt_keep=1: the chip left powered\n");
	} else {
		power_off("the steps are done");
	}
out:
	say("bt: done (%u receive errors, %u bytes not a packet)\n", rx_errors, rx.dropped_bytes);
	thread_terminate(current_thread());
}

void
hakios_bcm4361_bt_init(void)
{
	uint32_t steps = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("bt", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (pe_arm_get_soc_base_phys() != EXYNOS_SOC_BASE_PA) {
		return;                 /* not the Exynos 9810's (the POCO's bt=N is hakios.bt.c's) */
	}
	if (steps > 7) {
		steps = 7;
	}
	if (kernel_thread_start(bt_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
