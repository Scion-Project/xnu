/*
 * Copyright (c) 2000-2007 Apple Inc. All rights reserved.
 */
/*
 * Copyright (c) 1987, 1988 NeXT, Inc.
 *
 * HISTORY 7-Jan-93  Mac Gillon (mgillon) at NeXT Integrated POSIX support
 *
 * 12-Aug-87  John Seamons (jks) at NeXT Ported to NeXT.
 */

/*
 * Indirect driver for console.
 */
#include <kern/locks.h>
#include <machine/cons.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/ioctl.h>
#include <sys/tty.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/uio_internal.h>
#include <pexpert/pexpert.h>

static struct tty      *_constty;               /* current console device */
static LCK_GRP_DECLARE(_constty_lock_grp, "constty");
static LCK_MTX_DECLARE(_constty_lock, &_constty_lock_grp);

struct tty *
copy_constty(void)
{
	struct tty *result = NULL;
	lck_mtx_lock(&_constty_lock);
	if (_constty != NULL) {
		ttyhold(_constty);
		result = _constty;
	}
	lck_mtx_unlock(&_constty_lock);
	return result;
}

struct tty *
set_constty(struct tty *new_tty)
{
	struct tty *old_tty = NULL;
	lck_mtx_lock(&_constty_lock);
	old_tty = _constty;
	_constty = new_tty;
	if (_constty) {
		ttyhold(_constty);
	}
	lck_mtx_unlock(&_constty_lock);

	return old_tty;
}

/*
 * The km driver supplied the default console device for the systems
 * (usually a raw frame buffer driver, but potentially a serial driver).
 */
extern struct tty *km_tty[1];

/*
 * cdevsw[] entries for the console device driver
 */
int cnopen(__unused dev_t dev, int flag, int devtype, proc_t pp);
int cnclose(__unused dev_t dev, int flag, int mode, proc_t pp);
int cnread(__unused dev_t dev, struct uio *uio, int ioflag);
int cnwrite(__unused dev_t dev, struct uio *uio, int ioflag);
int cnioctl(__unused dev_t dev, u_long cmd, caddr_t addr, int flg, proc_t p);
int cnselect(__unused dev_t dev, int flag, void * wql, proc_t p);

int
cnopen(__unused dev_t dev, int flag, int devtype, struct proc *pp)
{
	int error;
	struct tty *constty = copy_constty();
	if (constty) {
		dev = constty->t_dev;
	} else {
		dev = km_tty[0]->t_dev;
	}
	error = (*cdevsw[major(dev)].d_open)(dev, flag, devtype, pp);
	if (constty != NULL) {
		ttyfree(constty);
	}
	return error;
}


int
cnclose(__unused dev_t dev, int flag, int mode, struct proc *pp)
{
	int error;
	struct tty *constty = copy_constty();
	if (constty) {
		dev = constty->t_dev;
	} else {
		dev = km_tty[0]->t_dev;
	}
	error = (*cdevsw[major(dev)].d_close)(dev, flag, mode, pp);
	if (constty != NULL) {
		ttyfree(constty);
	}
	return error;
}


int
cnread(__unused dev_t dev, struct uio *uio, int ioflag)
{
	int error;
	struct tty *constty = copy_constty();
	if (constty) {
		dev = constty->t_dev;
	} else {
		dev = km_tty[0]->t_dev;
	}
	error = (*cdevsw[major(dev)].d_read)(dev, uio, ioflag);
	if (constty != NULL) {
		ttyfree(constty);
	}
	return error;
}


#if defined(ARM64_BOARD_CONFIG_STARLTE)
/*
 * sweetcontee=1: a copy of what userland writes to /dev/console (daemons' stderr, our injected
 * libraries' lines, uncaught-exception text) goes into the kernel log ring too, so 0370's card log
 * (sdlog=1) carries it, each line prefixed "UCON ". First 1 KiB of each write, from its first iovec.
 */
extern void log_putc(char);
static void
sweet_contee(struct uio *uio)
{
	static int on = -1;
	static bool bol = true;
	if (on < 0) {
		on = 0;
		PE_parse_boot_argn("sweetcontee", &on, sizeof(on));
	}
	if (!on || uio_iovcnt(uio) < 1 || !UIO_SEG_IS_USER_SPACE(uio->uio_segflg)) {
		return;
	}
	char buf[1024];
	user_size_t n = uio_curriovlen(uio);
	if (n > sizeof(buf)) {
		n = sizeof(buf);
	}
	if (n == 0 || copyin(uio_curriovbase(uio), buf, (size_t)n) != 0) {
		return;
	}
	for (user_size_t i = 0; i < n; i++) {
		if (bol) {
			const char *pfx = "UCON ";
			while (*pfx) {
				log_putc(*pfx++);
			}
		}
		log_putc(buf[i]);
		bol = buf[i] == '\n';
	}
}
#endif

int
cnwrite(__unused dev_t dev, struct uio *uio, int ioflag)
{
	int error;
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	sweet_contee(uio);
#endif
	struct tty *constty = copy_constty();
	if (constty) {
		dev = constty->t_dev;
	} else {
		dev = km_tty[0]->t_dev;
	}
	error = (*cdevsw[major(dev)].d_write)(dev, uio, ioflag);
	if (constty != NULL) {
		ttyfree(constty);
	}
	return error;
}


int
cnioctl(__unused dev_t dev, u_long cmd, caddr_t addr, int flag, struct proc *p)
{
	int error;
	struct tty *constty = copy_constty();
	struct tty *freetp = NULL;
	if (constty) {
		dev = constty->t_dev;
	} else {
		dev = km_tty[0]->t_dev;
	}

	/*
	 * XXX This check prevents the cons.c code from being shared between
	 * XXX all architectures; it is probably not needed on ARM, either,
	 * XXX but I have no test platforms or ability to run a kernel.
	 *
	 * Superuser can always use this to wrest control of console
	 * output from the "virtual" console.
	 */
	if ((unsigned) cmd == TIOCCONS && constty) {
		error = proc_suser(p);
		if (error) {
			goto finish;
		}
		freetp = set_constty(NULL);
		if (freetp) {
			ttyfree(freetp);
			freetp = NULL;
		}
	} else {
		error = (*cdevsw[major(dev)].d_ioctl)(dev, cmd, addr, flag, p);
	}
finish:
	if (constty != NULL) {
		ttyfree(constty);
	}
	return error;
}


int
cnselect(__unused dev_t dev, int flag, void *wql, struct proc *p)
{
	int error;
	struct tty *constty = copy_constty();
	if (constty) {
		dev = constty->t_dev;
	} else {
		dev = km_tty[0]->t_dev;
	}
	error = (*cdevsw[major(dev)].d_select)(dev, flag, wql, p);
	if (constty != NULL) {
		ttyfree(constty);
	}
	return error;
}
