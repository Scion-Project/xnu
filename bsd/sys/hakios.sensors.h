/*
 * /dev/hakios-sensors: the phone's motion and environment sensors, sample by sample, on their way from the kernel driver
 * that reads them (the Galaxy S9's sensor hub: drivers/input/ssp/hakios.ssp.c, shub=6) to the code of ours in backboardd that
 * turns them into the HID events iOS's own consumers read (userland/src/inject.c). The same idea as /dev/hakios-touch
 * (sys/hakios.input.h): a queue, read() blocking until there is at least one sample and returning as many whole ones as
 * fit. The queue only fills while the device is open; a reader that falls 256 samples behind loses the oldest.
 *
 * Values are the sensor's own, as the hub reports them, in the phone's frame as Samsung's HAL gets them (the hub has
 * already turned the chips' axes into Android's: x right, y up, z out of the screen, with the device tree's positions):
 *   ACCEL      v[0..2] x, y, z; scale = the range in g (32768 counts are scale g: 8 g gives 4096 counts per g).
 *              Android's sign: lying flat, face up, z is +1 g (iOS reports -1 g there: the reader turns it around)
 *   GYRO       v[0..2] x, y, z in the hub's units (Samsung's HAL: 1/1000 degree per second... see SENSORS.md)
 *   MAG        v[0..2] x, y, z (calibrated), v[3] the accuracy 0-3
 *   PRESSURE   v[0] the pressure, v[1] the temperature (SENSORS.md has their units)
 *   LIGHT      v[0] lux, v[1] the colour temperature in K
 *   PROXIMITY  v[0] 1 when something is near, 0 when not; v[1] the raw reading
 */
#ifndef _SYS_HAKIOS_SENSORS_H_
#define _SYS_HAKIOS_SENSORS_H_

#include <stdint.h>

#define HAKIOS_SENSORS_NAME     "hakios-sensors"
#define HAKIOS_SENSORS_DEVICE   "/dev/hakios-sensors"

#define HAKIOS_SENSOR_ACCEL     1
#define HAKIOS_SENSOR_GYRO      2
#define HAKIOS_SENSOR_MAG       3
#define HAKIOS_SENSOR_PRESSURE  4
#define HAKIOS_SENSOR_LIGHT     5
#define HAKIOS_SENSOR_PROXIMITY 6

struct hakios_sensor_sample {
	uint64_t time;                  /* mach_absolute_time() when the kernel got it */
	uint64_t hub_ns;                /* the hub's own time stamp, in ns */
	uint32_t type;                  /* HAKIOS_SENSOR_* */
	int32_t scale;                  /* ACCEL: the range in g; else 0 */
	int32_t v[4];
};

#ifdef KERNEL
void hakios_ssp_init(void);             /* boot-arg shub=N on the Galaxy S9: its sensor hub (also in sys/hakios.input.h) */
void hakios_ssp_console(const char *arg); /* the USB console's "sensors" (hakios.dwc3.c) */
#endif

#endif /* _SYS_HAKIOS_SENSORS_H_ */
