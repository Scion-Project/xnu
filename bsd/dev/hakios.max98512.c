#define HAKIOS_SOC_FIXED
/*
 * PORT NOTE (Scion's iOS 27 tree, the fork's scion/xnu-12377, xnu-12377 STARLTE, S-Boot -> Starbite shim -> XNU). From Scion-Project/
 * scion-kernel drivers/audio/codec/hakios.max98512.c (APSL; HakiOS/Scion), whose head follows unchanged. Changed only so:
 *   - started from IOKitBSDInit.cpp (hakios_max98512_init(), next to hakios_bcm4361_bt_init()) instead of STARTUP;
 *     hakios_max98512_quiesce() is called from loader_return() (starlte_usbcon.c);
 *   - the battery's temperature (Scion's hakios_battery_temperature(), its fuel gauge's) is not known in this tree:
 *     -273 ("not known"), so unmute() always sets the warm brownout thresholds (0x30, 0x10), as Scion does whenever
 *     the temperature is not known;
 *   - ABOX's driver is in this kernel (bsd/dev/hakios.exynos_abox.c): HAKIOS_EXYNOS_ABOX defined here.
 * The bus is this tree's bsd/dev/hakios.exynos_hsi2c.c (Scion's, the same header).
 */
/*
 * The Galaxy S9's speakers (starlte, Exynos 9810): two Maxim MAX98512 amplifiers, 0x39 and 0x3A on the HSI2C of USI07 of
 * PERIC1 (0x108C0000), fed over I2S by UAIF1 of ABOX, the SoC's audio block, which drives both clocks. Only with boot-arg
 * max98512=N on this phone (the SoC's registers at 0x10000000). Every address and number below is this phone's.
 *
 * The bus (the phone's device tree: hsi2c@108C0000, alias hsi2c18, and what its clocks and pins name): the controller at
 * 0x108C0000, its input clock 200 MHz (default-clk), 400 kHz (no clock-frequency: i2c-exynos5.c's fast mode); its clocks
 * USI07 and GATE_USI07 of CMU_PERIC1 (0x10800000): the gates GATE_CLK_PERIC1_USI07_USI +0x203C,
 * RSTnSYNC_CLK_PERIC1_USI07_USI +0x2094, USI07_USI's IPCLK +0x20E4 and PCLK +0x20E8, the Q-channel +0x3038, the divider
 * +0x181C (7 bits), PERIC1's IP mux +0x0120; the USI's mode word usi@10811024 (usi_v2_mode "i2c": the words go two to a
 * USI from USI06's at 0x1081101C, as USI10's at 0x1081103C is the flashlight's bus); SCL gpp4-4 and SDA gpp4-5, function
 * 2 (hsi2c18-bus), in PERIC1's GPIO (0x10830000, bank gpp4 at +0: CON [23:16]).
 *
 * The amplifiers (max98512@39 under it): one codec of Android's over two chips, the main one at 0x39 (maxim,sub_reg 0x3A
 * the other, maxim,mono_stereo 1). In the stereo mode 0x39 plays I2S channel 0 (PCM_RX_EN_A 0x01, PCM_TO_SPK_MONOMIX_A
 * and _B 0x00 0x00) and 0x3A channel 1 (0x02, 0x41 0x01). Which speaker each drives is not in the tree; that 0x39 is the
 * top one (the earpiece, a speaker in stereo) is read from the driver: its receiver gain ("Rcv Digital Gain") is set on
 * 0x39 alone, its mono mode on 0x39 plays at the receiver's gain, and its brownout current limit for 0x39 (0x16) is the
 * lower (0x3E for 0x3A; the tree sets neither). No reset or enable pin and no supply in the tree: the node's pins are the
 * bus's (max98512-i2c: gpp4-4/5, their pull in power-down) and the two interrupts' (amp-int: gpm2-0 and gpm3-0 of CMGP),
 * left alone here (nothing waits on them). maxim,registers-of-amp and maxim,vmon_slot are read by nothing in the driver;
 * there are no vmon/imon slot numbers, so the IV sense would be the driver's own (slot 0 for 0x39, 1 for 0x3A,
 * interleaved: interleave_mode 1), and it stays off here.
 *   Two revisions, with different maps: REV_ID (0x0402) at 0x40 or above is the first, else REV_ID at 0x0600 at 0x40 or
 *   above is the "B", whose registers from 0x20 sit elsewhere (address(): +3, +1, +2, -2, -18, -13 by range;
 *   GLOBAL_SHDN 0x0500, SOFT_RESET 0x0501). Addresses are 16 bits (the high byte first), values 8.
 *
 * The format (the machine driver exynos9810_madera.c, its UAIF1 link): SND_SOC_DAIFMT_I2S | NB_NF | CBS_CFS, I2S with
 * neither clock inverted and the amplifiers slaves of both. ABOX's side (abox_if.c: abox_uaif_set_fmt, _hw_params) is
 * WS_MODE 0, BCLK_POLARITY 1, WS_POLAR 0, MODE 1 (master), SBIT_MAX the width less one and BCLK = rate * channels *
 * width, the width and the channels being ABOX's (abox_hw_params_fixup_helper). In the MAX98512 (max98512_dai_set_fmt,
 * _dai_hw_params, _set_clock): PCM_MASTER_MODE [1:0] 0, PCM_MODE_CFG [2] 0 (the BCLK edge), [5:3] 0 (I2S), [7:6] the
 * width (1 16, 2 24, 3 32 bits), PCM_CLK_SETUP [3:0] the BCLKs a frame (2 32, 3 48, 4 64), PCM_SR_SETUP1 [3:0] and
 * PCM_SR_SETUP2 [7:4] 8 (48 kHz), PCM_SR_SETUP2 [3:0] 5 (24 kHz, the IV ADC's when interleaved), SPK_SRC_SEL [1:0] 0
 * (the PCM input), PDM_RX_CTRL [0] 0.
 *
 * What is written, in what order (the first revision's numbering; in Android's order, in which each register goes to
 * 0x39 and then to 0x3A):
 *   configure: start()'s steps 1-3 when not done already for its width, and max98512=2 at boot (as for 48 kHz, 2 x 16 bits)
 *     GLOBAL_SHDN 0; SOFT_RESET 1, then REV_ID answering again and GLOBAL_SHDN [0], AMP_EN [0] reading 0
 *       (max98512_probe: its software reset; the shutdown before it is this driver's)
 *     MEAS_EN 0 (the IV sense off: Android turns it on for DSM, which does not run here); PCM_TX_HIZ_CTRL_A/B 0xFF;
 *       MONOMIX_A/B (above); AMP_VOL_CTRL the level (below) and SPK_GAIN 0, muted (Android's probe: 0x40 and 5);
 *       AMP_DSP_CFG 0x01 (the DC blocker, the volume ramp); MEAS_DSP_CFG 0xF7; BOOST_CTRL0 0x1C (10 V), BOOST_CTRL1
 *       0x3E, BOOST_CTRL3 0x01; MEAS_ADC_CFG 0x07, MEAS_ADC_BASE 0x00 0x0B, ADC_CH0-2_DIVIDE 2, ADC_CH0-2_FILT_CFG 0x0C;
 *       SQUELCH 0x10; BROWNOUT_EN 0x07, LVL_HOLD 0xFF, LVL1-3_THRESH 0xA0 0x30 0x10, THRESH_HYSTERYSIS 0x0F,
 *       AMP_LIMITER_ATK_REL 0x76; LVL1-3_CUR_LIMIT 0x16 (0x39) and 0x3E (0x3A) with LVL1-3_AMP1_CTRL1 6, 9, 12 and
 *       CTRL2-3 0, LVL4_CUR_LIMIT 0, LVL4_AMP1_CTRL1 0; ENV_TRACK_VOUT_HEADROOM 0x0A, ENV_TRACK_CTRL 0x01;
 *       BOOST_BYPASS_1 0x05, _2 0x00 (automatic bypass) when the top one is a B and 0x02 (off) when not, _3 0x01
 *       (max98512_probe; its write of 0x10 to ENV_TRACK_BOOST_VOUT_READ, a reading by its name and volatile in B's
 *       map, is left out)
 *     PCM_TX_HIZ_CTRL_A 0xFF, PCM_TX_EN_A/B 0: nothing sent back on the bus (max98512_reinit_reg; adc_config's IV
 *       slots are not opened); CLK_MON 0
 *     with a B among them: INT_EN1, INT_EN2, IRQ_CTRL 0, PCM_MASTER_MODE 0, PCM_CLK_SETUP 0x22, SSM_CFG 0x05,
 *       BROWNOUT_AMP_GAIN_ATK_REL 0, ENV_TRACK_BOOST_VOUT_DELAY, _REL_RATE, _HOLD_RATE 0 (max98512_probe)
 *     the format, above (max98512_dai_set_fmt, max98512_dai_hw_params, max98512_set_clock)
 *   unmute: start()'s steps 4-6 (max98512_dai_mute_stream -> __max98512_spk_enable, the stereo mode)
 *     SPK_GAIN [2:0] and AMP_VOL_CTRL [6:0], the level; MEAS_EN [1:0] 0; BROWNOUT_LVL2/3_THRESH 0x30 0x10 (0x40 0x20
 *     with the battery at 5 C or under: Android compares tenths of a degree with 50); BOOST_BYPASS_2 again; then 0x39's
 *     PCM_RX_EN_A, MONOMIX_A/B, AMP_EN [0] 1, AMP_EN [7:6] 2 (DEM), GLOBAL_SHDN [0] 1, and the same for 0x3A
 *   mute: stop() (max98512_spk_enable(0))
 *     GLOBAL_SHDN [0] 0, AMP_EN [0] 0, MEAS_EN [1:0] 0, then 15 ms with the clocks still running, for the ramp down
 *
 * The level. Android's speaker is +15 dB (maxim,spk-gain 5; SPK_GAIN [2:0] is 0 mute, then +3 to +18 dB in 3 dB steps) at
 * 0 dB digital (0x40; AMP_VOL_CTRL [6:0] is -16 dB and 0.25 dB a step), its receiver +3 dB at -3 dB (maxim,spk-gain-rcv
 * 1, maxim,digital-gain-rcv 0x34). Here both start at the receiver's level, 15 dB under Android's speaker.
 * max98512_gain=N raises the bottom one (1 to 5: +3 to +15 dB); the top one stays at the receiver's, being the earpiece.
 * Nothing ever sets more than SPK_GAIN 5 at 0 dB: the allow-list caps both registers (and BOOST_CTRL0 at Android's 10 V).
 *
 * The speakers' protection. Android's is Maxim's DSM on ABOX's DSP (dsm.bin), from the amplifiers' current and voltage
 * sense. None of that runs here, so the IV sense stays off, and what is left is the amplifiers' own (the DC blocker, the
 * brownout limiter, the boost's current limit, their thermal shutdown), the low level above, and whatever the feeder of
 * UAIF1 does to the samples: drivers/audio/hakios.guard.c (the POCO's: a high-pass, power and peak limits) is not in
 * this path; it would have to be built for the S9 and run on what is fed to UAIF1.
 *
 * Every write goes through one function that takes only the registers above (rules[]), none beyond its largest value,
 * and reads it back; a write that does not read back, or a bus that fails, ends writing for the run: both amplifiers
 * are shut down (GLOBAL_SHDN, AMP_EN) and start() refuses from then on. Waits are by the clock (deadline_us), not
 * delay(), which may sleep a tick. One transfer at a time (bus_lock), one start, stop or step at a time (amp_lock).
 *
 *   max98512=1    a look: the bus brought up (its clocks, the USI, the pins), both chips' revisions and registers;
 *                 nothing is written to them
 *   max98512=2    ... and both set up as for 48 kHz, 2 x 16 bits, shut down and muted; their registers again
 *   max98512=3    ... and both handed to ABOX (hakios_abox_set_amp): UAIF1's start turns them on, its stop off
 *   max98512_gain=N  the bottom speaker's gain at start, 1 to 5 (+3 to +15 dB; 1); the top one's is always 1
 *
 * Started on its own (STARTUP, as hakios.thermal.c is), not from hakios.input.c: that file is one of patch 1203's.
 *
 * Sources: the phone's device tree (hsi2c@108C0000 and max98512@39, usi@10811024, the hsi2c18-bus, max98512-i2c and
 * amp-int pins, the sound node's uaif@1, abox_uaif@1), Samsung's max98512.c and max98512.h, exynos9810_madera.c, abox.c
 * and abox_if.c, maxim_dsm_cal.c (the battery's temperature), i2c-exynos5.c, pinctrl-exynos.c (exynos9810_pin_banks7),
 * the Exynos 9810's CMU tables (cal-if/exynos9810/cmucal-sfr.c) and include/dt-bindings/clock/exynos9810.h of the S9's
 * kernel (GPL: read for these facts, not copied).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.max98512.h>
#include <sys/systm.h>
#include <sys/hakios.exynos_soc.h>
#define HAKIOS_EXYNOS_ABOX      1       /* (port) ABOX's driver is in this kernel: bsd/dev/hakios.exynos_abox.c */
#include <sys/hakios.log.h>
#include <sys/hakios.xnu.h>


/* the registers, in the first revision's numbering (a B's: address()) */
#define INT_EN1                 0x0A
#define INT_EN2                 0x0B
#define IRQ_CTRL                0x10
#define CLK_MON                 0x11
#define PCM_RX_EN_A             0x18
#define PCM_TX_EN_A             0x1A
#define PCM_TX_EN_B             0x1B
#define PCM_TX_HIZ_CTRL_A       0x1C
#define PCM_TX_HIZ_CTRL_B       0x1D
#define PCM_MODE_CFG            0x20
#define PCM_MASTER_MODE         0x21
#define PCM_CLK_SETUP           0x22
#define PCM_SR_SETUP1           0x23
#define PCM_SR_SETUP2           0x24
#define MONOMIX_A               0x25    /* PCM_TO_SPK_MONOMIX_A */
#define MONOMIX_B               0x26
#define PDM_RX_CTRL             0x34
#define AMP_VOL_CTRL            0x35
#define AMP_DSP_CFG             0x36
#define AMP_EN                  0x38
#define SPK_SRC_SEL             0x39
#define SPK_GAIN                0x3A
#define SSM_CFG                 0x3B
#define MEAS_EN                 0x3C
#define MEAS_DSP_CFG            0x3D
#define BOOST_CTRL0             0x3E
#define BOOST_CTRL3             0x3F
#define BOOST_CTRL1             0x40
#define MEAS_ADC_CFG            0x41
#define MEAS_ADC_BASE_MSB       0x42
#define MEAS_ADC_BASE_LSB       0x43
#define ADC_CH0_DIVIDE          0x44    /* CH1 0x45, CH2 0x46 */
#define ADC_CH0_FILT_CFG        0x47    /* CH1 0x48, CH2 0x49 */
#define SQUELCH                 0x4E
#define BROWNOUT_EN             0x50
#define BROWNOUT_LVL_HOLD       0x53
#define BROWNOUT_LVL1_THRESH    0x58
#define BROWNOUT_LVL2_THRESH    0x59
#define BROWNOUT_LVL3_THRESH    0x5A
#define BROWNOUT_HYSTERESIS     0x5C
#define BROWNOUT_LIMITER        0x5D    /* BROWNOUT_AMP_LIMITER_ATK_REL */
#define BROWNOUT_GAIN_ATK_REL   0x5E
#define BROWNOUT_LVL1_CUR_LIMIT 0x70    /* then its AMP1_CTRL1-3; level 2 at 0x74, 3 at 0x78, 4 at 0x7C */
#define ENV_HEADROOM            0x80    /* ENV_TRACK_VOUT_HEADROOM */
#define ENV_VOUT_DELAY          0x81
#define ENV_REL_RATE            0x82
#define ENV_HOLD_RATE           0x83
#define ENV_CTRL                0x84
#define BOOST_BYPASS_1          0x86
#define BOOST_BYPASS_2          0x87
#define BOOST_BYPASS_3          0x88
#define GLOBAL_SHDN             0x400
#define SOFT_RESET              0x401
#define REV_ID                  0x402
#define REV_ID_B                0x600   /* a B's REV_ID, where it is in the chip */
#define REV_ID_LEAST            0x40    /* a revision below this is not one */

#define SR_48000                0x8     /* PCM_SR_SETUP1 [3:0], PCM_SR_SETUP2 [7:4] */
#define SR_IV_24000             0x5     /* PCM_SR_SETUP2 [3:0], interleaved */

#define GAIN_RECEIVER           1       /* SPK_GAIN [2:0]: +3 dB, maxim,spk-gain-rcv */
#define GAIN_SPEAKER            5       /* +15 dB, maxim,spk-gain: Android's speaker, the most ever set */
#define VOL_RECEIVER            0x34    /* AMP_VOL_CTRL [6:0]: -3 dB, maxim,digital-gain-rcv */
#define VOL_MOST                0x40    /* 0 dB, Android's speaker */
#define BOOST_MOST              0x1C    /* BOOST_CTRL0: 10 V, Android's */

#define TRIES                   3       /* a transfer tried three times, as Android's driver does (MAX_TRY_COUNT) */
#define RESET_US                50000u  /* the most a software reset is given before the chip answers again */
#define RAMP_US                 16000u  /* after a mute: Android's 15 to 16 ms, the clocks still running */

static struct hakios_hsi2c bus = {
	.name = "max98512",
	.regs_pa = EXYNOS_PERIC1_USI07_HSI2C0_PA,                  /* hsi2c@108C0000: USI07 of PERIC1 */
	.usi_mode_pa = EXYNOS_PERIC1_USI07_MODE_PA,              /* usi@10811024: its mode word */
	.cmu_pa = EXYNOS_CMU_PERIC1_PA, .cmu_size = 0x4000,       /* CMU_PERIC1 */
	.gates = { 0x203C, 0x2094, 0x20E4, 0x20E8 }, .ngates = 4,
	.qch = 0x3038, .div = 0x181C, .mux = 0x0120,
	.pins_pa = EXYNOS_GPIO_PERIC1_PA,                  /* PERIC1's GPIO, bank gpp4: SCL pin 4, SDA pin 5 */
	.pins_mask = 0xffu << 16, .pins_want = 0x22u << 16,
	.ipclk_mhz = 200, .bus_hz = 400000,
};

static struct amp {
	const char *name;
	uint8_t addr;
	uint8_t rx_en, monomix_a, monomix_b;    /* its I2S channel, as Android's stereo mode sets it */
	uint8_t current_limit;                  /* BROWNOUT_LVL1-3_CUR_LIMIT */
	uint8_t gain, vol;                      /* its level: SPK_GAIN [2:0], AMP_VOL_CTRL [6:0] */
	bool b;                                 /* a MAX98512B, with its own map */
	uint8_t revision;
} amps[2] = {
	{ .name = "top", .addr = 0x39, .rx_en = 0x01, .monomix_a = 0x00, .monomix_b = 0x00, .current_limit = 0x16,
	  .gain = GAIN_RECEIVER, .vol = VOL_RECEIVER },
	{ .name = "bottom", .addr = 0x3A, .rx_en = 0x02, .monomix_a = 0x41, .monomix_b = 0x01, .current_limit = 0x3E,
	  .gain = GAIN_RECEIVER, .vol = VOL_RECEIVER },
};

/*
 * What may be written: every register this driver writes, and nothing else. `check' the bits a read-back must show as
 * written (0: SOFT_RESET, which clears itself); a value whose `capped' bits are above `cap' is refused (the gains, the
 * boost, the IV sense and the bus's transmit slots, which are never turned on).
 */
static const struct rule {
	uint16_t reg;
	uint8_t check, capped, cap;
} rules[] = {
	{ INT_EN1, 0xff, 0, 0 }, { INT_EN2, 0xff, 0, 0 }, { IRQ_CTRL, 0xff, 0, 0 }, { CLK_MON, 0xff, 0, 0 },
	{ PCM_RX_EN_A, 0xff, 0, 0 }, { PCM_TX_EN_A, 0xff, 0xff, 0 }, { PCM_TX_EN_B, 0xff, 0xff, 0 },
	{ PCM_TX_HIZ_CTRL_A, 0xff, 0, 0 }, { PCM_TX_HIZ_CTRL_B, 0xff, 0, 0 },
	{ PCM_MODE_CFG, 0xff, 0, 0 }, { PCM_MASTER_MODE, 0xff, 0, 0 }, { PCM_CLK_SETUP, 0xff, 0, 0 },
	{ PCM_SR_SETUP1, 0xff, 0, 0 }, { PCM_SR_SETUP2, 0xff, 0, 0 }, { MONOMIX_A, 0xff, 0, 0 }, { MONOMIX_B, 0xff, 0, 0 },
	{ PDM_RX_CTRL, 0xff, 0, 0 },
	{ AMP_VOL_CTRL, 0xff, 0xff, VOL_MOST },         /* 0 dB at most, the volume location bit clear */
	{ AMP_DSP_CFG, 0xff, 0, 0 }, { AMP_EN, 0xff, 0, 0 }, { SPK_SRC_SEL, 0xff, 0, 0 },
	{ SPK_GAIN, 0xff, 0x77, GAIN_SPEAKER },         /* +15 dB at most, the PDM gain [6:4] 0 */
	{ SSM_CFG, 0xff, 0, 0 },
	{ MEAS_EN, 0xff, 0x03, 0 },                     /* the IV sense: off only */
	{ MEAS_DSP_CFG, 0xff, 0, 0 },
	{ BOOST_CTRL0, 0xff, 0xff, BOOST_MOST },        /* 10 V at most, PVDD's bit clear */
	{ BOOST_CTRL3, 0xff, 0, 0 }, { BOOST_CTRL1, 0xff, 0, 0 },
	{ MEAS_ADC_CFG, 0xff, 0, 0 }, { MEAS_ADC_BASE_MSB, 0xff, 0, 0 }, { MEAS_ADC_BASE_LSB, 0xff, 0, 0 },
	{ ADC_CH0_DIVIDE, 0xff, 0, 0 }, { ADC_CH0_DIVIDE + 1, 0xff, 0, 0 }, { ADC_CH0_DIVIDE + 2, 0xff, 0, 0 },
	{ ADC_CH0_FILT_CFG, 0xff, 0, 0 }, { ADC_CH0_FILT_CFG + 1, 0xff, 0, 0 }, { ADC_CH0_FILT_CFG + 2, 0xff, 0, 0 },
	{ SQUELCH, 0xff, 0, 0 }, { BROWNOUT_EN, 0xff, 0, 0 }, { BROWNOUT_LVL_HOLD, 0xff, 0, 0 },
	{ BROWNOUT_LVL1_THRESH, 0xff, 0, 0 }, { BROWNOUT_LVL2_THRESH, 0xff, 0, 0 }, { BROWNOUT_LVL3_THRESH, 0xff, 0, 0 },
	{ BROWNOUT_HYSTERESIS, 0xff, 0, 0 }, { BROWNOUT_LIMITER, 0xff, 0, 0 }, { BROWNOUT_GAIN_ATK_REL, 0xff, 0, 0 },
	{ 0x70, 0xff, 0, 0 }, { 0x71, 0xff, 0, 0 }, { 0x72, 0xff, 0, 0 }, { 0x73, 0xff, 0, 0 },
	{ 0x74, 0xff, 0, 0 }, { 0x75, 0xff, 0, 0 }, { 0x76, 0xff, 0, 0 }, { 0x77, 0xff, 0, 0 },
	{ 0x78, 0xff, 0, 0 }, { 0x79, 0xff, 0, 0 }, { 0x7A, 0xff, 0, 0 }, { 0x7B, 0xff, 0, 0 },
	{ 0x7C, 0xff, 0, 0 }, { 0x7D, 0xff, 0, 0 },
	{ ENV_HEADROOM, 0xff, 0, 0 }, { ENV_VOUT_DELAY, 0xff, 0, 0 }, { ENV_REL_RATE, 0xff, 0, 0 },
	{ ENV_HOLD_RATE, 0xff, 0, 0 }, { ENV_CTRL, 0xff, 0, 0 },
	{ BOOST_BYPASS_1, 0xff, 0, 0 }, { BOOST_BYPASS_2, 0xff, 0, 0 }, { BOOST_BYPASS_3, 0xff, 0, 0 },
	{ GLOBAL_SHDN, 0x01, 0, 0 }, { SOFT_RESET, 0x00, 0xff, 0x01 },
};

static LCK_GRP_DECLARE(max98512_lock_group, "max98512");
static LCK_MTX_DECLARE(bus_lock, &max98512_lock_group);        /* one transfer at a time */
static LCK_MTX_DECLARE(amp_lock, &max98512_lock_group);        /* one start, stop or step at a time; the state below */
static bool broken;             /* a write did not read back, or the bus failed: no more writing (but to shut down) */
static bool playing;            /* unmuted by start(), until stop() */
static uint32_t configured_bits;  /* configure() done for this width and nothing written since but mute/unmute: 0 none */
static unsigned starts, refusals;

/* a moment `us' microseconds from now, by the clock: a loop bounded by it spins, where delay() would sleep a tick */
static uint64_t
deadline_us(uint64_t us)
{
	uint64_t interval;

	nanoseconds_to_absolutetime(us * NSEC_PER_USEC, &interval);
	return mach_absolute_time() + interval;
}

/* a register's address in the chip: the first revision's numbering, moved for a B (max98512_remap_reg's ranges) */
static uint16_t
address(const struct amp *a, uint16_t reg)
{
	if (!a->b || reg < 0x20) {
		return reg;
	}
	if (reg <= 0x28) {
		return reg + 3;
	}
	if (reg >= 0x2B && reg <= 0x4C) {
		return reg + 1;
	}
	if (reg >= 0x4F && reg <= 0x53) {
		return reg + 2;
	}
	if (reg >= 0x58 && reg <= 0x5F) {
		return reg - 2;
	}
	if (reg >= 0x70 && reg <= 0x7F) {
		return reg - 18;
	}
	if (reg >= 0x80 && reg <= 0x8D) {
		return reg - 13;
	}
	if (reg == GLOBAL_SHDN || reg == SOFT_RESET) {
		return reg + 0x100;
	}
	if (reg == REV_ID) {
		return REV_ID_B;
	}
	return reg;
}

/* ---- the bus: 16-bit addresses, high byte first ---- */

static uint32_t
raw_write(const struct amp *a, uint16_t at, uint8_t value)
{
	uint32_t error = 1;

	for (int i = 0; i < TRIES && error != 0; i++) {
		uint8_t buf[3] = { (uint8_t)(at >> 8), (uint8_t)at, value };

		lck_mtx_lock(&bus_lock);
		error = hakios_hsi2c_message(&bus, a->addr, false, buf, sizeof(buf), true);
		lck_mtx_unlock(&bus_lock);
	}
	return error;
}

static uint32_t
raw_read(const struct amp *a, uint16_t at, uint8_t *value)
{
	uint32_t error = 1;

	for (int i = 0; i < TRIES && error != 0; i++) {
		uint8_t buf[2] = { (uint8_t)(at >> 8), (uint8_t)at };

		lck_mtx_lock(&bus_lock);
		error = hakios_hsi2c_message(&bus, a->addr, false, buf, sizeof(buf), false);
		if (error == 0) {
			error = hakios_hsi2c_message(&bus, a->addr, true, value, 1, true);
		}
		lck_mtx_unlock(&bus_lock);
	}
	return error;
}

static uint32_t
get(const struct amp *a, uint16_t reg, uint8_t *value)
{
	return raw_read(a, address(a, reg), value);
}

static const struct rule *
rule_for(uint16_t reg)
{
	for (unsigned i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
		if (rules[i].reg == reg) {
			return &rules[i];
		}
	}
	return NULL;
}

/*
 * The one way a register is written: a register of rules[] alone, within its cap, read back (`check': the bits that
 * must read back as written). A refusal or a failure ends writing for the run; `even_broken' is shutting down, which
 * goes on regardless.
 */
static bool
write_checked(const struct amp *a, uint16_t reg, uint8_t value, uint8_t check, bool even_broken)
{
	const struct rule *r = rule_for(reg);
	uint16_t at = address(a, reg);
	uint8_t back = 0;
	uint32_t error;

	if (r == NULL || (value & r->capped) > r->cap) {
		say("max98512: %s: register %#x may not be given %#x: refused, and no more writing for this run\n", a->name, reg,
		    value);
		broken = true;
		return false;
	}
	if (broken && !even_broken) {
		return false;
	}
	error = raw_write(a, at, value);
	if (r->check == 0) {
		return true;                    /* SOFT_RESET: it clears itself; reset() looks whether it took */
	}
	if (error == 0) {
		error = raw_read(a, at, &back);
	}
	if (error != 0 || ((back ^ value) & check & r->check) != 0) {
		say("max98512: %s (%#x): register %#x (%#x in the chip): wrote %#x, read back %#x (bus %#x): no more writing "
		    "for this run\n", a->name, a->addr, reg, at, value, back, error);
		broken = true;
		return false;
	}
	return true;
}

static bool
put(const struct amp *a, uint16_t reg, uint8_t value)
{
	return write_checked(a, reg, value, 0xff, false);
}

static bool
put_bits(const struct amp *a, uint16_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now;

	if (broken) {
		return false;
	}
	if (get(a, reg, &now) != 0) {
		say("max98512: %s (%#x): register %#x cannot be read (bus %#x): no more writing for this run\n", a->name, a->addr,
		    reg, bus.last_error);
		broken = true;
		return false;
	}
	return write_checked(a, reg, (uint8_t)((now & ~mask) | (value & mask)), mask, false);
}

/* each register to the top one and then to the bottom one, as Android's driver writes to both */
static bool
both(uint16_t reg, uint8_t value)
{
	return put(&amps[0], reg, value) && put(&amps[1], reg, value);
}

static bool
both_bits(uint16_t reg, uint8_t mask, uint8_t value)
{
	return put_bits(&amps[0], reg, mask, value) && put_bits(&amps[1], reg, mask, value);
}

static bool
each(uint16_t reg, uint8_t top, uint8_t bottom)
{
	return put(&amps[0], reg, top) && put(&amps[1], reg, bottom);
}

/* bits cleared in both, whatever happened before: the way to silence (an amplifier that does not answer is passed by) */
static void
clear_both(uint16_t reg, uint8_t mask)
{
	for (unsigned i = 0; i < 2; i++) {
		uint8_t now = 0;

		if (get(&amps[i], reg, &now) != 0) {
			say("max98512: %s (%#x): register %#x cannot be read (bus %#x) to be cleared\n", amps[i].name, amps[i].addr,
			    reg, bus.last_error);
			broken = true;
			continue;
		}
		(void)write_checked(&amps[i], reg, (uint8_t)(now & ~mask), mask, true);
	}
}

/* ---- the chips ---- */

/* which chip answers at an address (max98512_i2c_probe's revision check); false: none this driver knows */
static bool
identify(struct amp *a)
{
	uint8_t id = 0, id_b = 0;

	if (raw_read(a, REV_ID, &id) != 0) {
		say("max98512: %s (%#x): no answer (bus %#x)\n", a->name, a->addr, bus.last_error);
		return false;
	}
	if (id >= REV_ID_LEAST) {
		a->b = false;
		a->revision = id;
	} else if (raw_read(a, REV_ID_B, &id_b) == 0 && id_b >= REV_ID_LEAST) {
		a->b = true;
		a->revision = id_b;
	} else {
		say("max98512: %s (%#x): REV_ID %#x, and %#x at 0x600: not a MAX98512 this driver knows\n", a->name, a->addr, id,
		    id_b);
		return false;
	}
	say("max98512: %s (%#x): MAX98512%s, revision %#x\n", a->name, a->addr, a->b ? "B" : "", a->revision);
	return true;
}

static bool
readable(const struct amp *a, uint16_t at)
{
	if (a->b) {
		return (at >= 0x01 && at <= 0x81) || at == 0x500 || at == 0x501 || at == REV_ID_B;
	}
	return (at >= 0x01 && at <= 0x28) || (at >= 0x2B && at <= 0x53) || (at >= 0x58 && at <= 0x5F) ||
	       (at >= 0x70 && at <= 0x88) || (at >= GLOBAL_SHDN && at <= REV_ID);
}

/* `count' registers from `first', as they are in the chip ("--" not readable, "??" no answer) */
static void
dump_row(const struct amp *a, uint16_t first, unsigned count)
{
	char line[16 * 3 + 1];
	unsigned used = 0;

	line[0] = 0;
	for (unsigned i = 0; i < count && i < 16; i++) {
		uint16_t at = (uint16_t)(first + i);
		uint8_t value = 0;

		if (!readable(a, at)) {
			snprintf(line + used, sizeof(line) - used, " --");
		} else if (raw_read(a, at, &value) != 0) {
			snprintf(line + used, sizeof(line) - used, " ??");
		} else {
			snprintf(line + used, sizeof(line) - used, " %02x", (unsigned)value);
		}
		used += 3;
	}
	say("max98512: %s %#06x:%s\n", a->name, (unsigned)first, line);
}

static void
dump(const struct amp *a, const char *when)
{
	say("max98512: %s (%#x), %s: its registers, as the chip numbers them\n", a->name, a->addr, when);
	for (uint16_t row = 0; row < 0x90; row += 0x10) {
		dump_row(a, row, 16);
	}
	if (a->b) {
		dump_row(a, 0x500, 2);
		dump_row(a, REV_ID_B, 1);
	} else {
		dump_row(a, GLOBAL_SHDN, 3);
	}
}

/* the software reset (max98512_probe), and the chip answering again in shutdown */
static bool
reset(const struct amp *a)
{
	uint8_t id = 0, shdn = 0xff, en = 0xff;
	uint64_t deadline;

	if (!write_checked(a, SOFT_RESET, 0x01, 0, false)) {
		return false;
	}
	for (deadline = deadline_us(RESET_US);;) {
		if (get(a, REV_ID, &id) == 0 && id == a->revision) {
			break;
		}
		if (mach_absolute_time() > deadline) {
			say("max98512: %s (%#x): no answer %u ms after the software reset (REV_ID %#x): no more writing for this run\n",
			    a->name, a->addr, RESET_US / 1000, id);
			broken = true;
			return false;
		}
	}
	if (get(a, GLOBAL_SHDN, &shdn) != 0 || get(a, AMP_EN, &en) != 0 || (shdn & 1) != 0 || (en & 1) != 0) {
		say("max98512: %s (%#x): after the software reset GLOBAL_SHDN %#x, AMP_EN %#x: not shut down; no more writing "
		    "for this run\n", a->name, a->addr, shdn, en);
		broken = true;
		return false;
	}
	return true;
}

/*
 * Steps 1-3 of start(): both reset and set up, and the format; shut down and muted at the end. (Android's
 * max98512_probe, then the stream's max98512_dai_set_fmt, _dai_hw_params and _set_clock: see the top of this file.)
 */
static bool
configure(uint32_t slot_bits, uint32_t channels)
{
	uint8_t width = slot_bits == 16 ? 1 : slot_bits == 24 ? 2 : 3;
	uint8_t bclks = (uint8_t)(channels * slot_bits / 16);   /* 32, 48, 64 BCLKs a frame: 2, 3, 4 */
	uint8_t bypass = amps[0].b ? 0x00 : 0x02;
	bool any_b = amps[0].b || amps[1].b;

	return both(GLOBAL_SHDN, 0) && reset(&amps[0]) && reset(&amps[1]) &&
	       both(MEAS_EN, 0) && both(PCM_TX_HIZ_CTRL_A, 0xff) && both(PCM_TX_HIZ_CTRL_B, 0xff) &&
	       each(MONOMIX_A, amps[0].monomix_a, amps[1].monomix_a) && each(MONOMIX_B, amps[0].monomix_b, amps[1].monomix_b) &&
	       each(AMP_VOL_CTRL, amps[0].vol, amps[1].vol) && both(SPK_GAIN, 0) &&
	       both(AMP_DSP_CFG, 0x01) && both(MEAS_DSP_CFG, 0xf7) &&
	       both(BOOST_CTRL0, 0x1c) && both(BOOST_CTRL1, 0x3e) && both(BOOST_CTRL3, 0x01) &&
	       both(MEAS_ADC_CFG, 0x07) && both(MEAS_ADC_BASE_MSB, 0x00) && both(MEAS_ADC_BASE_LSB, 0x0b) &&
	       both(ADC_CH0_DIVIDE, 0x02) && both(ADC_CH0_DIVIDE + 1, 0x02) && both(ADC_CH0_DIVIDE + 2, 0x02) &&
	       both(ADC_CH0_FILT_CFG, 0x0c) && both(ADC_CH0_FILT_CFG + 1, 0x0c) && both(ADC_CH0_FILT_CFG + 2, 0x0c) &&
	       both(SQUELCH, 0x10) && both(BROWNOUT_EN, 0x07) && both(BROWNOUT_LVL_HOLD, 0xff) &&
	       both(BROWNOUT_LVL1_THRESH, 0xa0) && both(BROWNOUT_LVL2_THRESH, 0x30) && both(BROWNOUT_LVL3_THRESH, 0x10) &&
	       both(BROWNOUT_HYSTERESIS, 0x0f) && both(BROWNOUT_LIMITER, 0x76) &&
	       each(0x70, amps[0].current_limit, amps[1].current_limit) && both(0x71, 0x06) && both(0x72, 0) && both(0x73, 0) &&
	       each(0x74, amps[0].current_limit, amps[1].current_limit) && both(0x75, 0x09) && both(0x76, 0) && both(0x77, 0) &&
	       each(0x78, amps[0].current_limit, amps[1].current_limit) && both(0x79, 0x0c) && both(0x7A, 0) && both(0x7B, 0) &&
	       both(0x7C, 0) && both(0x7D, 0) &&
	       both(ENV_HEADROOM, 0x0a) && both(ENV_CTRL, 0x01) &&
	       both(BOOST_BYPASS_1, 0x05) && both(BOOST_BYPASS_2, bypass) && both(BOOST_BYPASS_3, 0x01) &&
	       /* max98512_adc_config's start; the IV slots stay closed */
	       both(PCM_TX_HIZ_CTRL_A, 0xff) && both(PCM_TX_EN_A, 0) && both(PCM_TX_EN_B, 0) && both(CLK_MON, 0) &&
	       (!any_b || (both(INT_EN1, 0) && both(INT_EN2, 0) && both(IRQ_CTRL, 0) && both(PCM_MASTER_MODE, 0) &&
	       both(PCM_CLK_SETUP, 0x22) && both(SSM_CFG, 0x05) && both(BROWNOUT_GAIN_ATK_REL, 0) &&
	       both(ENV_VOUT_DELAY, 0) && both(ENV_REL_RATE, 0) && both(ENV_HOLD_RATE, 0))) &&
	       /* the format: max98512_dai_set_fmt (I2S, NB_NF, CBS_CFS), max98512_dai_hw_params, max98512_set_clock */
	       both_bits(PCM_MASTER_MODE, 0x03, 0x00) && both_bits(PCM_MODE_CFG, 0x04, 0x00) &&
	       put_bits(&amps[0], PCM_RX_EN_A, amps[0].rx_en, amps[0].rx_en) &&
	       put_bits(&amps[1], PCM_RX_EN_A, amps[1].rx_en, amps[1].rx_en) &&
	       both_bits(PCM_MODE_CFG, 0x38, 0x00) && both_bits(SPK_SRC_SEL, 0x03, 0x00) && both_bits(PDM_RX_CTRL, 0x01, 0x00) &&
	       both_bits(PCM_MODE_CFG, 0xc0, (uint8_t)(width << 6)) && both_bits(PCM_SR_SETUP1, 0x0f, SR_48000) &&
	       both_bits(PCM_SR_SETUP2, 0xf0, SR_48000 << 4) && both_bits(PCM_SR_SETUP2, 0x0f, SR_IV_24000) &&
	       both_bits(PCM_CLK_SETUP, 0x0f, bclks);
}

/* (port) no fuel gauge in this tree: the battery's temperature is not known */
static int
hakios_battery_temperature(void)
{
	return -273;
}

/* steps 4-6 of start(): the speaker path on, the level, the global enable (__max98512_spk_enable, the stereo mode) */
static bool
unmute(void)
{
	int celsius = hakios_battery_temperature();
	bool cold = celsius != -273 && celsius <= 5;
	bool ok;

	ok = put_bits(&amps[0], SPK_GAIN, 0x07, amps[0].gain) && put_bits(&amps[0], AMP_VOL_CTRL, 0x7f, amps[0].vol) &&
	    put_bits(&amps[1], SPK_GAIN, 0x07, amps[1].gain) && put_bits(&amps[1], AMP_VOL_CTRL, 0x7f, amps[1].vol) &&
	    both_bits(MEAS_EN, 0x03, 0) &&
	    both(BROWNOUT_LVL2_THRESH, cold ? 0x40 : 0x30) && both(BROWNOUT_LVL3_THRESH, cold ? 0x20 : 0x10) &&
	    both(BOOST_BYPASS_2, amps[0].b ? 0x00 : 0x02);
	for (unsigned i = 0; ok && i < 2; i++) {
		const struct amp *a = &amps[i];

		ok = put(a, PCM_RX_EN_A, a->rx_en) && put(a, MONOMIX_A, a->monomix_a) && put(a, MONOMIX_B, a->monomix_b) &&
		    put_bits(a, AMP_EN, 0x01, 0x01) && put_bits(a, AMP_EN, 0xc0, 0x80) && put_bits(a, GLOBAL_SHDN, 0x01, 0x01);
	}
	return ok;
}

/* shut down, in Android's order (max98512_spk_enable(0)), whatever happened before; then the ramp down, by the clock */
static void
mute(void)
{
	uint64_t deadline;

	clear_both(GLOBAL_SHDN, 0x01);
	clear_both(AMP_EN, 0x01);
	clear_both(MEAS_EN, 0x03);
	for (deadline = deadline_us(RAMP_US); mach_absolute_time() < deadline;) {
	}
}

/* ---- ABOX's hook ---- */

/* after UAIF1's clocks run: both reset, set up and unmuted; 0, EINVAL (a format other than this phone's) or EIO */
static int
amp_start(uint32_t rate, uint32_t slot_bits, uint32_t channels)
{
	uint64_t t0 = mach_absolute_time(), us;
	bool again = configured_bits == slot_bits;
	int error = 0;

	lck_mtx_lock(&amp_lock);
	if (broken) {
		error = EIO;
	} else if (rate != 48000 || channels != 2 || (slot_bits != 16 && slot_bits != 24 && slot_bits != 32)) {
		error = EINVAL;
	} else {
		if (playing) {
			mute();
			playing = false;
		}
		/*
		 * The reset and the whole set-up only when the amplifiers are not set up for this width already (the first
		 * start, or after max98512=2's set-up for another one): a start is then only unmute(), a few writes, so that
		 * the beginning of a short sound (a click; ABOX starts the amplifiers when iOS's first loud frames arrive)
		 * is not spent on I2C. stop() (mute) leaves the set-up as it is.
		 */
		if ((configured_bits == slot_bits || configure(slot_bits, channels)) && unmute()) {
			configured_bits = slot_bits;
			playing = true;
			starts++;
		} else {
			configured_bits = 0;
			mute();
			error = EIO;
		}
	}
	if (error != 0) {
		refusals++;
	}
	lck_mtx_unlock(&amp_lock);
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &us);
	us /= 1000;
	if (error == 0) {
		say("max98512: on: %u Hz, %u x %u bits, I2S; top +%d dB, bottom +%d dB, both at -3 dB digital (start %u, %s, %llu us)\n",
		    rate, channels, slot_bits, 3 * amps[0].gain, 3 * amps[1].gain, starts, again ? "unmuted only" : "set up first", us);
	} else {
		say("max98512: start (%u Hz, %u x %u bits) refused: %s (%u so far)\n", rate, channels, slot_bits,
		    error == EINVAL ? "not 48000 Hz, 2 channels of 16, 24 or 32 bits" : broken ? "writing has ended for this run" :
		    "the amplifiers did not take it", refusals);
	}
	return error;
}

/* before UAIF1's clocks stop: both shut down, and 16 ms for the ramp down */
static void
amp_stop(void)
{
	lck_mtx_lock(&amp_lock);
	mute();
	playing = false;
	lck_mtx_unlock(&amp_lock);
}

/*
 * On the way back to the loader (loader_return(), a panic's way out too): both shut down, if start() left them playing.
 * One processor is left and no lock may be taken, so the bus is used directly: reset first (a transfer may have been cut
 * off with the processor that made it), then GLOBAL_SHDN's enable bit cleared in each, then the ramp down with UAIF1's
 * clocks still running (hakios_exynos_abox_quiesce() stops them next). Nothing else is written.
 */
void
hakios_max98512_quiesce(void)
{
	uint64_t deadline;

	if (!playing) {
		return;
	}
	hakios_hsi2c_reset(&bus);
	for (unsigned i = 0; i < 2; i++) {
		uint16_t at = address(&amps[i], GLOBAL_SHDN);
		uint8_t buf[3] = { (uint8_t)(at >> 8), (uint8_t)at, 0 };
		uint8_t now = 0;
		uint32_t error = hakios_hsi2c_message(&bus, amps[i].addr, false, buf, 2, false);

		if (error == 0) {
			error = hakios_hsi2c_message(&bus, amps[i].addr, true, &now, 1, true);
		}
		if (error == 0) {
			buf[2] = now & ~0x01;
			error = hakios_hsi2c_message(&bus, amps[i].addr, false, buf, 3, true);
		}
		say("max98512: at the run's end: %s (%#x) GLOBAL_SHDN %#x -> %#x%s\n", amps[i].name, amps[i].addr, now,
		    now & ~0x01, error == 0 ? "" : ": the bus FAILED");
	}
	for (deadline = deadline_us(RAMP_US); mach_absolute_time() < deadline;) {
	}
	playing = false;
}

/*
 * What the amplifiers say of themselves (ABOX's reports while it plays, and the console's "abox"): the interrupts' raw
 * and live state (INT_RAW1-3, INT_STATE1-3: the clock monitor, thermal and over-current flags), the enables, the level,
 * the measurement ADC's three channels (MEAS_ADC_CH0-2_READ, with MEAS_ADC_CFG 0x07 all three converting), the
 * brownout state and the boost's output as the envelope tracker reads it (ENV_TRACK_BOOST_VOUT_READ): with a tone
 * playing the boost follows the signal, so a reading that moves away from the idle one is the amplifier's own word
 * that it plays. Only read; no lock but the bus's (a start or stop may run at the same time: the numbers are then
 * those of a moment of it).
 */
static void
amp_status(const char *when)
{
	static const uint16_t regs[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, GLOBAL_SHDN, AMP_EN, SPK_GAIN, AMP_VOL_CTRL,
		                         0x4A, 0x4B, 0x4C, 0x4F, 0x85 };
	char line[sizeof(regs) / sizeof(regs[0]) * 3 + 1];

	for (unsigned i = 0; i < 2; i++) {
		const struct amp *a = &amps[i];
		unsigned used = 0;

		if (a->revision == 0) {
			continue;
		}
		for (unsigned r = 0; r < sizeof(regs) / sizeof(regs[0]); r++) {
			uint8_t v = 0;

			snprintf(line + used, sizeof(line) - used, get(a, regs[r], &v) == 0 ? " %02x" : " ??", (unsigned)v);
			used += 3;
		}
		say("max98512: %s, %s: INT_RAW1-3 INT_STATE1-3, GLOBAL_SHDN AMP_EN SPK_GAIN AMP_VOL, ADC_CH0-2 BROWNOUT, BOOST_VOUT:%s\n",
		    a->name, when, line);
	}
}

static const struct hakios_abox_amp amp_hook = {
	.name = "max98512", .start = amp_start, .stop = amp_stop, .status = amp_status,
};

/* ---- the steps ---- */

static void
amp_thread(void *arg, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg;
	bool ok;

	say("max98512: 1: map the bus (USI07 of PERIC1), its clocks on, USI07 and its pins to I2C; the two chips' revisions\n");
	if (!hakios_hsi2c_map(&bus, say)) {
		goto out;
	}
	say("max98512: 1: USI07's divider %u (7 bits)\n", (*(volatile uint32_t *)(bus.cmu + bus.div) & 0x7f) + 1);
	lck_mtx_lock(&bus_lock);
	hakios_hsi2c_power(&bus);
	hakios_hsi2c_reset(&bus);
	lck_mtx_unlock(&bus_lock);
	lck_mtx_lock(&amp_lock);
	ok = identify(&amps[0]);
	ok = identify(&amps[1]) && ok;          /* (both asked, whatever the first said) */
	for (unsigned i = 0; i < 2; i++) {
		if (amps[i].revision != 0) {
			dump(&amps[i], "as found");
		}
	}
	lck_mtx_unlock(&amp_lock);
	if (!ok) {
		say("max98512: 1: both amplifiers are needed: nothing is written\n");
		goto out;
	}
	if (steps < 2) {
		goto out;
	}
	say("max98512: 2: both reset and set up as for 48 kHz, 2 x 16 bits, I2S; shut down and muted\n");
	lck_mtx_lock(&amp_lock);
	ok = configure(16, 2);
	configured_bits = ok ? 16 : 0;
	if (!ok) {
		mute();
	}
	for (unsigned i = 0; i < 2; i++) {
		dump(&amps[i], ok ? "set up, muted" : "after a failure, shut down");
	}
	lck_mtx_unlock(&amp_lock);
	if (!ok || steps < 3) {
		goto out;
	}
	say("max98512: 3: handed to ABOX's UAIF1: its start turns them on (top +%d dB, bottom +%d dB, both at -3 dB), its "
	    "stop off\n", 3 * amps[0].gain, 3 * amps[1].gain);
	hakios_abox_set_amp(&amp_hook);
#ifndef HAKIOS_EXYNOS_ABOX
	say("max98512: 3: this kernel has no ABOX driver: nothing will turn the amplifiers on\n");
#endif
out:
	thread_terminate(current_thread());
}

void
hakios_max98512_init(void)
{
	uint32_t steps = 0, gain;
	thread_t thread;

	if (!PE_parse_boot_argn("max98512", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (pe_arm_get_soc_base_phys() != EXYNOS_SOC_BASE_PA) {
		return;                 /* not the Exynos 9810's */
	}
	if (PE_parse_boot_argn("max98512_gain", &gain, sizeof(gain)) && gain >= GAIN_RECEIVER && gain <= GAIN_SPEAKER) {
		amps[1].gain = (uint8_t)gain;
	}
	if (kernel_thread_start(amp_thread, (void *)(uintptr_t)(steps > 3 ? 3 : steps), &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
