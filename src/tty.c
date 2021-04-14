/* Finit TTY handling
 *
 * Copyright (c) 2013       Mattias Walström <lazzer@gmail.com>
 * Copyright (c) 2013-2021  Joachim Wiberg <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <ctype.h>		/* isdigit() */
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <lite/lite.h>

#include "config.h"		/* Generated by configure script */
#include "finit.h"
#include "conf.h"
#include "helpers.h"
#include "service.h"
#include "tty.h"
#include "util.h"
#include "utmp-api.h"

char *tty_canonicalize(char *dev)
{
	static char path[80];
	struct stat st;

	if (!dev)
		return NULL;

	strlcpy(path, dev, sizeof(path));
	if (stat(path, &st)) {
		if (!strncmp(path, _PATH_DEV, strlen(_PATH_DEV))) {
		unavailable:
			_d("TTY %s not available at the moment, registering anyway.", path);
			return path;
		}

		snprintf(path, sizeof(path), "%s%s", _PATH_DEV, dev);
		if (stat(path, &st))
			goto unavailable;
	}

	if (!S_ISCHR(st.st_mode))
		return NULL;

	return path;
}

/*
 * The @console syntax is a wildcard to match the system console(s) given
 * on the kernel cmdline.  As such it can match multiple, or even none.
 */
int tty_isatcon(char *dev)
{
	return dev && !strcmp(dev, "@console");
}

int tty_atcon(char *buf, size_t len)
{
	FILE *fp;

	fp = fopen("/sys/class/tty/console/active", "r");
	if (!fp) {
		_e("Cannot find system console, is sysfs not mounted?");
		errno = ENOENT;
		return -1;
	}

	if (!fgets(buf, len, fp)) {
		fclose(fp);
		return -1;
	}

	chomp(buf);
	fclose(fp);

	return 0;
}

/**
 * tty_parse_args - Parse cmdline args for a tty
 * @cmd: command or tty
 *
 * A Finit tty line can use the internal getty implementation or an
 * external one, like the BusyBox getty for instance.  This function
 * determines which one to use based on a leading '/dev' prefix.  If
 * a leading '/dev' is encountered the remaining options must be in
 * the following sequence:
 *
 *     tty [!1-9,S] <DEV> [BAUD[,BAUD,...]] [noclear] [nowait] [TERM]
 *
 * Otherwise the leading prefix must be the full path to an existing
 * getty implementation, with it's arguments following:
 *
 *     tty [!1-9,S] </path/to/getty> [ARGS] [noclear] [nowait]
 *
 * Different getty implementations prefer the TTY device argument in
 * different order, so take care to investigate this first.
 */
int tty_parse_args(char *cmd, struct tty *tty)
{
	char  *dev = NULL;
	size_t i;

	do {
		_d("token %s", cmd);
		if (!strcmp(cmd, "noclear"))
			tty->noclear = 1;
		else if (!strcmp(cmd, "nowait"))
			tty->nowait  = 1;
		else if (!strcmp(cmd, "nologin"))
			tty->nologin = 1;
		else if (!strcmp(cmd, "notty"))
			tty->notty = 1;	/* for fallback shell */
		else
			tty->args[tty->num++] = cmd;

		cmd = strtok(NULL, " \t");
	} while (cmd && tty->num < NELEMS(tty->args));

	/* skip /dev probe, we just want a bríngup shell */
	if (tty->notty)
		return 0;

	/* Iterate over all args */
	for (i = 0; i < tty->num; i++) {
		_d("Checking arg %s for dev and cmd ...", tty->args[i]);
		/* 
		 * First, figure out if built-in or external getty
		 * tty [12345] /dev/ttyAMA0 115200 noclear vt220		# built-in
		 * tty [12345] /sbin/getty -L 115200 @console vt100 noclear	# external
		 */
		if ((!tty->cmd && !dev) || (tty->cmd && !dev)) {
			if (!strcmp(tty->args[i], "@console"))
				dev = tty->args[i];
			if (!strncmp(tty->args[i], "/dev", 4))
				dev = tty->args[i];
			if (!strncmp(tty->args[i], "tty", 3) || !strcmp(tty->args[i], "console"))
				dev = tty->args[i];
			if (!access(tty->args[i], X_OK))
				tty->cmd = tty->args[i];

			/* The first arg must be one of the above */
			continue;
		}

		/* Built-in getty args */
		if (!tty->cmd && dev) {
			_d("Found dev %s for built-in getty", dev);
			if (isdigit(tty->args[i][0])) {
				tty->baud = tty->args[i];
				continue;
			}

			/*
			 * Last arg, if not anything else, is the value
			 * to be used for the TERM environment variable.
			 */
			if (i + 1 == tty->num)
				tty->term = tty->args[i];
		}
	}

	if (!tty_isatcon(dev))
		tty->dev = tty_canonicalize(dev);

	if (!tty->dev) {
		_e("Incomplete or non-existing TTY device given, cannot register.");
		return errno = EINVAL;
	}

	_d("Registering %s getty on TTY %s at %s baud with term %s", tty->cmd ? "external" : "built-in",
	   tty->dev, tty->baud ?: "0", tty->term ?: "N/A");

	return 0;
}

static int tty_exist(char *dev)
{
	int fd, result;
	struct termios c;

	fd = open(dev, O_RDWR);
	if (-1 == fd)
		return 1;

	/* XXX: Add check for errno == EIO? */
	result = tcgetattr(fd, &c);
	close(fd);

	return result;
}

int tty_exec(svc_t *tty)
{
	char *dev;
	int rc;

	if (tty->notty) {
		/*
		 * Become session leader and set controlling TTY
		 * to enable Ctrl-C and job control in shell.
		 */
		setsid();
		ioctl(STDIN_FILENO, TIOCSCTTY, 1);

		prctl(PR_SET_NAME, "finitsh", 0, 0, 0);
		return execl(_PATH_BSHELL, _PATH_BSHELL, NULL);
	}

	dev = tty_canonicalize(tty->dev);
	if (!dev) {
		_d("%s: Cannot find TTY device: %s", tty->dev, strerror(errno));
		return EX_CONFIG;
	}

	if (tty_exist(dev)) {
		_d("%s: Not a valid TTY: %s", dev, strerror(errno));
		return EX_OSFILE;
	}

	if (tty->nologin) {
		_d("%s: Starting /bin/sh ...", dev);
		return run_sh(dev, tty->noclear, tty->nowait, tty->rlimit);
	}

	_d("%s: Starting %sgetty ...", dev, !tty->cmd ? "built-in " : "");
	if (!strcmp(tty->cmd, "tty"))
		rc = run_getty(dev, tty->baud, tty->term, tty->noclear, tty->nowait, tty->rlimit);
	else
//		rc = run_getty2(dev, tty->cmd, tty->args, tty->noclear, tty->nowait, tty->rlimit);
		rc = -1;

	return rc;
}

/*
 * Fallback shell if no TTYs are active
 */
int tty_fallback(char *file)
{
	svc_t *svc, *iter = NULL;
	size_t num = 0;

	for (svc = svc_iterator(&iter, 1); svc; svc = svc_iterator(&iter, 0)) {
		if (!svc_is_tty(svc) || svc_is_removed(svc))
			continue;
		num++;
	}

#ifdef FALLBACK_SHELL
	char line[32] = "tty [12345789] notty noclear";

	if (!num) {
		_d("No TTY active in configuration, enabling fallback shell.");
		return service_register(SVC_TYPE_TTY, line, global_rlimit, file);
	}
#endif

	return num == 0;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
