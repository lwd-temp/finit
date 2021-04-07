/* Finit service monitor, task starter and generic API for managing svc_t
 *
 * Copyright (c) 2008-2010  Claudio Matsuoka <cmatsuoka@gmail.com>
 * Copyright (c) 2008-2021  Joachim Wiberg <troglobit@gmail.com>
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

#include "config.h"		/* Generated by configure script */

#include <ctype.h>		/* isblank() */
#include <sched.h>		/* sched_yield() */
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <net/if.h>
#include <lite/lite.h>
#include <wordexp.h>

#include "cgroup.h"
#include "conf.h"
#include "cond.h"
#include "finit.h"
#include "helpers.h"
#include "pid.h"
#include "private.h"
#include "sig.h"
#include "service.h"
#include "sm.h"
#include "tty.h"
#include "util.h"
#include "utmp-api.h"
#include "schedule.h"

static struct wq work = {
	.cb = service_worker,
};

static void svc_set_state(svc_t *svc, svc_state_t new);

/**
 * service_timeout_cb - libuev callback wrapper for service timeouts
 * @w:      Watcher
 * @arg:    Callback argument, from init
 * @events: Error, or ready to read/write (N/A for relative timers)
 *
 * Run callback registered when calling service_timeout_after().
 */
static void service_timeout_cb(uev_t *w, void *arg, int events)
{
	svc_t *svc = arg;

	/* Ignore any UEV_ERROR, we're a one-shot cb so just run it. */
	if (svc->timer_cb)
		svc->timer_cb(svc);
}

/**
 * service_timeout_after - Call a function after some time has elapsed
 * @svc:     Service to use as argument to the callback
 * @timeout: Timeout, in milliseconds
 * @cb:      Callback function
 *
 * After @timeout milliseconds has elapsed, call @cb() with @svc as the
 * argument.
 *
 * Returns:
 * POSIX OK(0) on success, non-zero on error.
 */
static int service_timeout_after(svc_t *svc, int timeout, void (*cb)(svc_t *svc))
{
	if (svc->timer_cb)
		return -EBUSY;

	svc->timer_cb = cb;
	return uev_timer_init(ctx, &svc->timer, service_timeout_cb, svc, timeout, 0);
}

/**
 * service_timeout_cancel - Cancel timeout associated with service
 * @svc: Service whose timeout to cancel
 *
 * If a timeout is associated with @svc, cancel it.
 *
 * Returns:
 * POSIX OK(0) on success, non-zero on error.
 */
static int service_timeout_cancel(svc_t *svc)
{
	int err;

	if (!svc->timer_cb)
		return 0;

	err = uev_timer_stop(&svc->timer);
	svc->timer_cb = NULL;

	return err;
}

/*
 * Redirect stdin to /dev/null => all reads by process = EOF
 * https://www.freedesktop.org/software/systemd/man/systemd.exec.html#Logging%20and%20Standard%20Input/Output
 */
static int stdin_redirect(void)
{
	int fd;

	fd = open("/dev/null", O_RDONLY | O_APPEND);
	if (-1 != fd) {
		dup2(fd, STDIN_FILENO);
		return close(fd);
	}

	return -1;
}

/*
 * Redirect output to a file, e.g., /dev/null, or /dev/console
 */
static int fredirect(const char *file)
{
	int fd;

	fd = open(file, O_WRONLY | O_APPEND);
	if (-1 != fd) {
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		return close(fd);
	}

	return -1;
}

/*
 * Fallback in case we don't even have logger on the system.
 * XXX: we should parse 'prio' here to get facility.level
 */
static void fallback_logger(char *ident, char *prio)
{
	int facility = LOG_DAEMON;
	int level = LOG_NOTICE;
	char buf[256];

	prctl(PR_SET_NAME, "finitlog", 0, 0, 0);
	openlog(ident, LOG_NOWAIT | LOG_PID, facility);
	while ((fgets(buf, sizeof(buf), stdin)))
		syslog(level, "%s", buf);

	closelog();
}

/*
 * Redirect output to syslog using the command line logit tool
 */
static int lredirect(svc_t *svc)
{
	pid_t pid;
	int fd;

	/*
	 * Open PTY to connect to logger.  A pty isn't buffered
	 * like a pipe, and it eats newlines so they aren't logged
	 */
	fd = posix_openpt(O_RDWR);
	if (fd == -1) {
		svc->log.enabled = 0;
		return -1;
	}
	if (grantpt(fd) == -1 || unlockpt(fd) == -1) {
		close(fd);
		svc->log.enabled = 0;
		return -1;
	}

	pid = fork();
	if (pid == 0) {
		int fds;
		char *tag  = basename(svc->cmd);
		char *prio = "daemon.info";

		fds = open(ptsname(fd), O_RDONLY);
		close(fd);
		if (fds == -1)
			_exit(0);
		dup2(fds, STDIN_FILENO);

		/* Reset signals */
		sig_unblock();

		if (!whichp(LOGIT_PATH)) {
			logit(LOG_INFO, LOGIT_PATH " missing, using syslog for %s instead", svc->name);
			fallback_logger(tag, prio);
			_exit(0);
		}

		if (svc->log.file[0] == '/') {
			char sz[20], num[3];

			snprintf(sz, sizeof(sz), "%d", logfile_size_max);
			snprintf(num, sizeof(num), "%d", logfile_count_max);

			execlp(LOGIT_PATH, "logit", "-f", svc->log.file, "-n", sz, "-r", num, NULL);
			_exit(1);
		}

		if (svc->log.ident[0])
			tag = svc->log.ident;
		if (svc->log.prio[0])
			prio = svc->log.prio;

		execlp(LOGIT_PATH, "logit", "-t", tag, "-p", prio, NULL);
		_exit(1);
	}

	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);

	return close(fd);
}

/*
 * Handle redirection of process output, if enabled
 */
static int redirect(svc_t *svc)
{
	stdin_redirect();

	if (svc->log.enabled) {
		if (svc->log.null)
			return fredirect("/dev/null");
		if (svc->log.console)
			return fredirect(console());

		return lredirect(svc);
	} else if (debug)
		return fredirect(console());
#ifdef REDIRECT_OUTPUT
	else
		return fredirect("/dev/null");
#endif

	return 0;
}

/*
 * Source environment file, if it exists
 * Note: must be called from privsepped child
 */
static void source_env(svc_t *svc)
{
	char buf[LINE_SIZE];
	char *line;
	size_t len;
	FILE *fp;
	char *fn;

	fn = svc_getenv(svc);
	if (!fn)
		return;

	/* Warning in service_start() after svc_checkenv() */
	fp = fopen(fn, "r");
	if (!fp)
		return;

	line = buf;
	len = sizeof(buf);
	while (fgets(line, len, fp)) {
		char *key = chomp(line);
		char *value, *end;

		/* skip any leading whitespace */
		while (isspace(*key))
			key++;

		/* skip comments */
		if (*key == '#' || *key == ';')
			continue;

		/* find end of line */
		end = key;
		while (*end)
			end++;

		/* strip trailing whitespace */
		if (end > key) {
			end--;
			while (isspace(*end))
				*end-- = 0;
		}

		value = strchr(key, '=');
		if (!value)
			continue;
		*value++ = 0;

		/* strip leading whitespace from value */
		while (isspace(*value))
			value++;

		/* unquote value, if quoted */
		if (value[0] == '"' || value[0] == '\'') {
			char q = value[0];

			if (*end == q) {
				value = &value[1];
				*end = 0;
			}
		}

		/* find end of key */
		end = key;
		while (*end)
			end++;

		/* strip trailing whitespace */
		if (end > key) {
			end--;
			while (isspace(*end))
				*end-- = 0;
		}

		setenv(key, value, 1);
	}

	fclose(fp);
}

static int is_norespawn(void)
{
	return  sig_stopped()            ||
		fexist("/mnt/norespawn") ||
		fexist("/tmp/norespawn");
}

/* used for process group name, derived from originating filename,
 * so to group multiple services, place them in the same .conf
 */
static char *group_name(svc_t *svc, char *buf, size_t len)
{
	char *ptr;

	if (!svc->file[0])
		return svc_ident(svc, buf, len);

	ptr = strrchr(svc->file, '/');
	if (ptr)
		ptr++;
	else
		ptr = svc->file;

	strlcpy(buf, ptr, len);
	ptr = strstr(buf, ".conf");
	if (ptr)
		*ptr = 0;

	return buf;
}

/**
 * service_start - Start service
 * @svc: Service to start
 *
 * Returns:
 * 0 if the service was successfully started. Non-zero otherwise.
 */
static int service_start(svc_t *svc)
{
	int result = 0, do_progress = 1;
	sigset_t nmask, omask;
	char grnam[80];
	pid_t pid;
	size_t i;

	if (!svc)
		return 1;

	/* Ignore if finit is SIGSTOP'ed */
	if (is_norespawn())
		return 1;

	/* Don't try and start service if it doesn't exist. */
	if (!whichp(svc->cmd)) {
		logit(LOG_WARNING, "%s: missing or not in $PATH", svc->cmd);
		svc_missing(svc);
		return 1;
	}

	/* Unlike systemd we do not allow starting service if env is missing, unless - */
	if (!svc_checkenv(svc)) {
		logit(LOG_WARNING, "%s: missing env file %s", svc->cmd, svc->env);
		svc_missing(svc);
		return 1;
	}

	if (svc_is_sysv(svc))
		logit(LOG_CONSOLE | LOG_NOTICE, "Calling '%s start' ...", svc->cmd);

	if (!svc->desc[0])
		do_progress = 0;

	if (do_progress) {
		if (svc_is_daemon(svc) || svc_is_sysv(svc))
			print_desc("Starting ", svc->desc);
		else
			print_desc("", svc->desc);
	}

	/* Declare we're waiting for svc to create its pidfile */
	svc_starting(svc);

	/* Block SIGCHLD while forking.  */
	sigemptyset(&nmask);
	sigaddset(&nmask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &nmask, &omask);

	pid = fork();
	if (pid == 0) {
		int status;
		char *home = NULL;
#ifdef ENABLE_STATIC
		int uid = 0; /* XXX: Fix better warning that dropprivs is disabled. */
		int gid = 0;
#else
		int uid = getuser(svc->username, &home);
		int gid = getgroup(svc->group);
#endif
		char *args[MAX_NUM_SVC_ARGS + 1];

		/* Set configured limits */
		for (int i = 0; i < RLIMIT_NLIMITS; i++) {
			if (setrlimit(i, &svc->rlimit[i]) == -1)
				logit(LOG_WARNING,
				      "%s: rlimit: Failed setting %s",
				      svc->cmd, rlim2str(i));
		}

		/* Set desired user+group */
		if (gid >= 0)
			(void)setgid(gid);

		if (uid >= 0) {
			(void)setuid(uid);

			/* Set default path for regular users */
			if (uid > 0)
				setenv("PATH", _PATH_DEFPATH, 1);
			if (home) {
				setenv("HOME", home, 1);
				if (chdir(home))
					(void)chdir("/");
			}
		}

		/* Source any environment from env:/path/to/file */
		source_env(svc);

		if (!svc_is_sysv(svc)) {
			wordexp_t we = { 0 };
			int rc;

			if ((rc = wordexp(svc->cmd, &we, 0))) {
				_e("%s: failed wordexp(%s): %d", svc->cmd, svc->cmd, rc);
			nomem:
				wordfree(&we);
				_exit(1);
			}

			for (i = 0; i < MAX_NUM_SVC_ARGS; i++) {
				char *arg = svc->args[i];
				size_t len = strlen(arg);
				char str[len + 2];
				char ch = *arg;

				if (len == 0)
					break;

				/*
				 * Escape forbidden characters in wordexp()
				 * but allowed in Finit run/task stanzas,
				 *
				 * XXX: escapes only leading characters ...
				 */
				if (strchr("|<>&:", ch))
					sprintf(str, "\\");
				else
					str[0] = 0;
				strlcat(str, arg, sizeof(str));

				if ((rc = wordexp(str, &we, WRDE_APPEND))) {
					_e("%s: failed wordexp(%s): %d", svc->cmd, str, rc);
					goto nomem;
				}
			}

			if (we.we_wordc > MAX_NUM_SVC_ARGS) {
				logit(LOG_ERR, "%s: too man args after expansion.", svc->cmd);
				goto nomem;
			}

			for (i = 0; i < we.we_wordc; i++) {
				if (strlen(we.we_wordv[i]) >= sizeof(svc->args[i])) {
					logit(LOG_ERR, "%s: expanded arg. '%s' too long", we.we_wordv[i]);
					rc = WRDE_NOSPACE;
					goto nomem;
				}

				/* overwrite the child's svc with expanded args */
				strlcpy(svc->args[i], we.we_wordv[i], sizeof(svc->args[i]));
				args[i] = svc->args[i];
			}
			wordfree(&we);
		} else {
			i = 0;
			args[i++] = svc->cmd;
			args[i++] = "start";
		}
		args[i] = NULL;

		redirect(svc);
		sig_unblock();

		/*
		 * The setsid() call takes care to detach the process
		 * from its controlling terminal, preventing daemons
		 * from leaking to the console, and allowing us to run
		 * such programs like `lxc-start -F` in the foreground
		 * to properly monitor them.
		 *
		 * If you find yourself here wanting to fix the output
		 * to the console at boot, for debugging or similar,
		 * have a look at redirect() and log.console instead.
		 */
		setsid();

		if (svc_is_runtask(svc))
			status = exec_runtask(args[0], &args[1]);
		else
			status = execvp(args[0], &args[1]);

		_exit(status);
	} else if (debug) {
		char buf[CMD_SIZE] = "";

		for (i = 0; i < MAX_NUM_SVC_ARGS; i++) {
			if (strlen(svc->args[i]) == 0)
				break;
			if (buf[0])
				strlcat(buf, " ", sizeof(buf));
			strlcat(buf, svc->args[i], sizeof(buf));
		}
		_d("Starting %s %s", svc->cmd, buf);
	}

	cgroup_service(group_name(svc, grnam, sizeof(grnam)), pid, &svc->cgroup);

	logit(LOG_CONSOLE | LOG_NOTICE, "Starting %s[%d]", svc_ident(svc, NULL, 0), pid);

	svc->pid = pid;
	svc->start_time = jiffies();

	switch (svc->type) {
	case SVC_TYPE_RUN:
		svc->status = complete(svc->cmd, pid);
		if (WIFEXITED(svc->status) && !WEXITSTATUS(svc->status))
			result = 0;
		else
			result = 1;
		svc->start_time = svc->pid = 0;
		svc->once++;
		svc_set_state(svc, SVC_STOPPING_STATE);
		break;

	case SVC_TYPE_SERVICE:
		pid_file_create(svc);
		break;

	default:
		break;
	}

	sigprocmask(SIG_SETMASK, &omask, NULL);
	if (do_progress)
		print_result(result);

	return result;
}

/**
 * service_kill - Forcefully terminate a service
 * @param svc  Service to kill
 *
 * Called when a service refuses to terminate gracefully.
 */
static void service_kill(svc_t *svc)
{
	service_timeout_cancel(svc);

	if (svc->pid <= 1) {
		/* Avoid killing ourselves or all processes ... */
		_d("%s: Aborting SIGKILL, already terminated.", svc->cmd);
		return;
	}

	_d("%s: Sending SIGKILL to pid:%d", pid_get_name(svc->pid, NULL, 0), svc->pid);
	logit(LOG_CONSOLE | LOG_NOTICE, "Stopping %s[%d], sending SIGKILL ...",
	      svc_ident(svc, NULL, 0), svc->pid);
	if (runlevel != 1)
		print_desc("Killing ", svc->desc);

	kill(-svc->pid, SIGKILL);

	/* Let SIGKILLs stand out, show result as [WARN] */
	if (runlevel != 1)
		print(2, NULL);
}

/*
 * Clean up any lingering state from dead/killed services
 */
static void service_cleanup(svc_t *svc)
{
	char *fn;

	fn = pid_file(svc);
	if (fn && remove(fn) && errno != ENOENT)
		logit(LOG_CRIT, "Failed removing service %s pidfile %s",
		      basename(svc->cmd), fn);

	/* No longer running, update books. */
	svc->oldpid = svc->pid;
	svc->start_time = svc->pid = 0;
}

/**
 * service_stop - Stop service
 * @svc: Service to stop
 *
 * Returns:
 * 0 if the service was successfully stopped. Non-zero otherwise.
 */
static int service_stop(svc_t *svc)
{
	int rc = 0;

	if (!svc)
		return 1;

	if (svc->state <= SVC_STOPPING_STATE)
		return 0;

	service_timeout_cancel(svc);

	if (!svc_is_sysv(svc)) {
		if (svc->pid <= 1)
			return 1;

		_d("Sending %s to pid:%d name:%s", sig_name(svc->sighalt),
		   svc->pid, pid_get_name(svc->pid, NULL, 0));
		logit(LOG_CONSOLE | LOG_NOTICE, "Stopping %s[%d], sending %s ...",
		      svc_ident(svc, NULL, 0), svc->pid, sig_name(svc->sighalt));
	} else {
		logit(LOG_CONSOLE | LOG_NOTICE, "Calling '%s stop' ...", svc->cmd);
	}

	svc_set_state(svc, SVC_STOPPING_STATE);

	if (runlevel != 1)
		print_desc("Stopping ", svc->desc);

	if (!svc_is_sysv(svc)) {
		if (svc->pid > 1) {
			/* Kill all children in the same proess group, e.g. logit */
			rc = kill(-svc->pid, svc->sighalt);

			/* PID lost or forking process never really started */
			if (rc == -1 && ESRCH == errno)
				service_cleanup(svc);
		} else
				service_cleanup(svc);
	} else {
		char *args[] = { svc->cmd, "stop", NULL };
		pid_t pid;

		pid = fork();
		switch (pid) {
		case 0:
			redirect(svc);
			exec_runtask(svc->cmd, args);
			_exit(0);
			break;
		case -1:
			_pe("Failed fork() to call sysv script '%s stop'", svc->cmd);
			rc = 1;
			break;
		default:
			rc = WEXITSTATUS(complete(svc->cmd, pid));
			break;
		}
	}

	if (runlevel != 1)
		print_result(rc);

	return rc;
}

/**
 * service_restart - Restart a service by sending %SIGHUP
 * @svc: Service to reload
 *
 * This function does some basic checks of the runtime state of Finit
 * and a sanity check of the @svc before sending %SIGHUP.
 *
 * Returns:
 * POSIX OK(0) or non-zero on error.
 */
static int service_restart(svc_t *svc)
{
	int do_progress = 1;
	pid_t lost = 0;
	int rc;

	/* Ignore if finit is SIGSTOP'ed */
	if (is_norespawn())
		return 1;

	if (!svc || !svc->sighup)
		return 1;

	if (svc->pid <= 1) {
		_d("Bad PID %d for %s, SIGHUP", svc->pid, svc->cmd);
		svc->start_time = svc->pid = 0;
		return 1;
	}

	/* Skip progress if desc disabled or bootstrap task */
	if (!svc->desc[0] || svc_in_runlevel(svc, 0))
		do_progress = 0;

	if (do_progress)
		print_desc("Restarting ", svc->desc);

	_d("Sending SIGHUP to PID %d", svc->pid);
	logit(LOG_CONSOLE | LOG_NOTICE, "Restarting %s[%d], sending SIGHUP ...",
	      svc_ident(svc, NULL, 0), svc->pid);
	rc = kill(svc->pid, SIGHUP);
	if (rc == -1 && errno == ESRCH) {
		/* nobody home, reset internal state machine */
		lost = svc->pid;
	} else {
		/* Declare we're waiting for svc to re-assert/touch its pidfile */
		svc_starting(svc);

		/* Service does not maintain a PID file on its own */
		if (svc_has_pidfile(svc)) {
			sched_yield();
			touch(pid_file(svc));
		}
	}

	if (do_progress)
		print_result(rc);

	if (lost)
		service_monitor(lost, 0);

	return rc;
}

/**
 * service_reload_dynamic - Called on SIGHUP, 'init q' or 'initctl reload'
 *
 * This function is called when Finit has recieved SIGHUP to reload
 * .conf files in /etc/finit.d.  It is responsible for starting,
 * stopping and reloading (forwarding SIGHUP) to processes affected.
 */
void service_reload_dynamic(void)
{
	sm_set_reload(&sm);
	sm_step(&sm);
}

/**
 * service_runlevel - Change to a new runlevel
 * @newlevel: New runlevel to activate
 *
 * Stops all services not in @newlevel and starts, or lets continue to run,
 * those in @newlevel.  Also updates @prevlevel and active @runlevel.
 */
void service_runlevel(int newlevel)
{
	if (!rescue && runlevel <= 1 && newlevel > 1)
		networking(1);

	sm_set_runlevel(&sm, newlevel);
	sm_step(&sm);

	if (!rescue && runlevel <= 1)
		networking(0);
}

/*
 * log:/path/to/logfile,priority:facility.level,tag:ident
 */
static void parse_log(svc_t *svc, char *arg)
{
	char *tok;

	tok = strtok(arg, ":, ");
	while (tok) {
		if (!strcmp(tok, "log"))
			svc->log.enabled = 1;
		else if (!strcmp(tok, "null") || !strcmp(tok, "/dev/null"))
			svc->log.null = 1;
		else if (!strcmp(tok, "console") || !strcmp(tok, "/dev/console"))
			svc->log.console = 1;
		else if (tok[0] == '/')
			strlcpy(svc->log.file, tok, sizeof(svc->log.file));
		else if (!strcmp(tok, "priority") || !strcmp(tok, "prio"))
			strlcpy(svc->log.prio, strtok(NULL, ","), sizeof(svc->log.prio));
		else if (!strcmp(tok, "tag") || !strcmp(tok, "identity") || !strcmp(tok, "ident"))
			strlcpy(svc->log.ident, strtok(NULL, ","), sizeof(svc->log.ident));

		tok = strtok(NULL, ":=, ");
	}
}

static void parse_env(svc_t *svc, char *env)
{
	if (!env)
		return;

	if (strlen(env) >= sizeof(svc->env)) {
		_e("%s: env file is too long (>%d chars)", svc->cmd, sizeof(svc->env));
		return;
	}

	strlcpy(svc->env, env, sizeof(svc->env));
}

static void parse_cgroup(svc_t *svc, char *cgroup)
{
	char *ptr = cgroup;

	if (!cgroup)
		return;

	if (cgroup[0] == '.') {
		ptr = strchr(cgroup, ':');
		if (ptr)
			*ptr++ = 0;
		strlcpy(svc->cgroup.name, &cgroup[1], sizeof(svc->cgroup.name));
		if (!ptr)
			return;
	}

	if (strlen(ptr) >= sizeof(svc->cgroup)) {
		_e("%s: cgroup settings too long (>%d chars)", svc->cmd, sizeof(svc->cgroup));
		return;
	}

	strlcpy(svc->cgroup.cfg, ptr, sizeof(svc->cgroup.cfg));
}

static void parse_sighalt(svc_t *svc, char *arg)
{
	int signo;

	signo = sig_num(arg);
	if (signo == -1)
		return;

	svc->sighalt = signo;
}

static void parse_killdelay(svc_t *svc, char *delay)
{
	const char *errstr;
	long long sec;

	sec = strtonum(delay, 1, 60, &errstr);
	if (errstr) {
		_e("%s: killdelay %s is %s (1-60)", svc->cmd, delay, errstr);
		return;
	}

	/* convert to msec */
	svc->killdelay = (int)(sec * 1000);
}

/*
 * name:<name>
 */
static void parse_name(svc_t *svc, char *arg)
{
	char *name = NULL;

	if (arg && !strncasecmp(arg, "name:", 5)) {
		name = arg + 5;
	} else {
		name = strrchr(svc->cmd, '/');
		name = name ? name + 1 : svc->cmd;
	}

	strlcpy(svc->name, name, sizeof(svc->name));

	/* Warn if svc generates same condition as an existing service */
	svc_validate(svc);
}

/**
 * parse_cmdline_args - Update the command line args in the svc struct
 *
 * strtok internal pointer must be positioned at first command line arg
 * when this function is called.
 *
 * Side effect: strtok internal pointer will be modified.
 */
static void parse_cmdline_args(svc_t *svc, char *cmd)
{
	int diff = 0;
	char sep = 0;
	char *arg;
	int i = 0;

	if (strcmp(svc->args[i], cmd))
		diff++;
	strlcpy(svc->args[i++], cmd, sizeof(svc->args[0]));

	/*
	 * Copy supplied args. Stop at MAX_NUM_SVC_ARGS-1 to allow the args
	 * array to be zero-terminated.
	 */
	for (i = 1; (arg = strtok(NULL, " ")) && i < (MAX_NUM_SVC_ARGS - 1);) {
		char prev[sizeof(svc->args[0])] = { 0 };
		char ch = arg[0];
		size_t len;

		if (!sep) {
			strlcpy(prev, svc->args[i], sizeof(prev));
			svc->args[i][0] = 0;
		}

		/* XXX: ugly string arg re-concatenation, fixme */
		if (ch == '"' || ch == '\'')
			sep = ch;
		else if (sep)
			strlcat(svc->args[i], " ", sizeof(svc->args[0]));

		strlcat(svc->args[i], arg, sizeof(svc->args[0]));

		/* string arg contained already? */
		len = strlen(arg);
		if (sep && len >= 1) {
			ch = arg[len - 1];
			if (ch != sep)
				continue;
		}

		if (strcmp(svc->args[i], prev))
			diff++;

		sep = 0;
		i++;
	}

	/*
	 * Clear remaining args in case they were set earlier.
	 * This also zero-terminates the args array.
	 */
	while (i < MAX_NUM_SVC_ARGS) {
		if (svc->args[i++][0]) {
			svc->args[i-1][0] = 0;
			diff++;
		}
	}
#if 0
	for (i = 0; i < MAX_NUM_SVC_ARGS; i++) {
		if (!svc->args[i][0])
			break;
		_d("%s ", svc->args[i]);
	}
#endif

	if (diff)
		_d("Modified args for %s detected", cmd);
	svc->args_dirty = (diff > 0);
}


/**
 * service_register - Register service, task or run commands
 * @type:   %SVC_TYPE_SERVICE(0), %SVC_TYPE_TASK(1), %SVC_TYPE_RUN(2)
 * @cfg:    Configuration, complete command, with -- for description text
 * @rlimit: Limits for this service/task/run, may be global limits
 * @file:   The file name service was loaded from
 *
 * This function is used to register commands to be run on different
 * system runlevels with optional username.  The @type argument details
 * if it's service to bo monitored/respawned (daemon), a one-shot task
 * or a command that must run in sequence and not in parallell, like
 * service and task commands do.
 *
 * The @line can optionally start with a username, denoted by an @
 * character. Like this:
 *
 *     service @username [!0-6,S] <!COND> /path/to/daemon arg -- Description
 *     task @username [!0-6,S] /path/to/task arg              -- Description
 *     run  @username [!0-6,S] /path/to/cmd arg               -- Description
 *
 * If the username is left out the command is started as root.  The []
 * brackets denote the allowed runlevels, if left out the default for a
 * service is set to [2-5].  Allowed runlevels mimic that of SysV init
 * with the addition of the 'S' runlevel, which is only run once at
 * startup.  It can be seen as the system bootstrap.  If a task or run
 * command is listed in more than the [S] runlevel they will be called
 * when changing runlevel.
 *
 * Services (daemons) also support an optional <!condition> argument.
 * This is for services that depend on another service, e.g. Quagga ripd
 * depends on zebra, or require a system gateway or interface to be up
 * before they are started.  Or restarted, or even SIGHUP'ed, when the
 * gateway changes or interfaces come and go.  The special case when a
 * service is declared with <!> means it does not support SIGHUP but
 * must be STOP/START'ed at system reconfiguration.
 *
 * Conditions can for example be: pid/NAME:ID for process dependencies,
 * net/<IFNAME>/up or net/<IFNAME>/exists.  The condition handling is
 * further described in doc/conditions.md, but worth mentioning here is
 * that the condition a services *provides* can be modified using the
 * :ID and name:foo syntax.
 *
 * For multiple instances of the same command, e.g. multiple DHCP
 * clients, the user must enter an ID, using the :ID syntax.
 *
 *     service :eth1 /sbin/udhcpc -i eth1
 *     service :eth2 /sbin/udhcpc -i eth2
 *
 * Without the :ID syntax, Finit replaces the first service line with
 * the contents of the second.  The :ID can be any string value and
 * defaults to "" (emtpy string).
 *
 * Returns:
 * POSIX OK(0) on success, or non-zero errno exit status on failure.
 */
int service_register(int type, char *cfg, struct rlimit rlimit[], char *file)
{
	char *cmd, *desc, *runlevels = NULL, *cond = NULL;
	char *username = NULL, *log = NULL, *pid = NULL;
	char *name = NULL, *halt = NULL, *delay = NULL;
	char *id = NULL, *env = NULL, *cgroup = NULL;
	int levels = 0;
	int manual = 0;
	char *line;
	svc_t *svc;

	if (!cfg) {
		_e("Invalid input argument");
		return errno = EINVAL;
	}

	line = strdupa(cfg);
	if (!line)
		return 1;

	desc = strstr(line, "-- ");
	if (desc) {
		*desc = 0;
		desc += 3;

		while (*desc && isblank(*desc))
			desc++;
	} else {
		int pos;

		/* Find "--\n" to denote empty/no description */
		pos = (int)strlen(line) - 2;
		if (pos > 0 && !strcmp(&line[pos], "--")) {
			line[pos] = 0;
			desc = &line[pos];
		}
	}

	cmd = strtok(line, " ");
	if (!cmd) {
	incomplete:
		_e("Incomplete service '%s', cannot register", cfg);
		return errno = ENOENT;
	}

	while (cmd) {
		if (cmd[0] == '@')	/* @username[:group] */
			username = &cmd[1];
		else if (cmd[0] == '[')	/* [runlevels] */
			runlevels = &cmd[0];
		else if (cmd[0] == '<')	/* <[!][cond][,cond..]> */
			cond = &cmd[1];
		else if (cmd[0] == ':')	/* :ID */
			id = &cmd[1];
		else if (!strncasecmp(cmd, "log", 3))
			log = cmd;
		else if (!strncasecmp(cmd, "pid", 3))
			pid = cmd;
		else if (!strncasecmp(cmd, "name:", 5))
			name = cmd;
		else if (!strncasecmp(cmd, "manual:yes", 10))
			manual = 1;
		else if (!strncasecmp(cmd, "halt:", 5))
			halt = &cmd[5];
		else if (!strncasecmp(cmd, "kill:", 5))
			delay = &cmd[5];
		else if (!strncasecmp(cmd, "env:", 4))
			env = &cmd[4];
		else if (!strncasecmp(cmd, "cgroup:", 7))
			cgroup = &cmd[7]; /* only settings */
		else if (!strncasecmp(cmd, "cgroup.", 7))
			cgroup = &cmd[6]; /* with group */
		else
			break;

		/* Check if valid command follows... */
		cmd = strtok(NULL, " ");
		if (!cmd)
			goto incomplete;
	}

	levels = conf_parse_runlevels(runlevels);
	if (runlevel > 0 && !ISOTHER(levels, 0)) {
		_d("Skipping %s, bootstrap is completed.", cmd);
		return 0;
	}

	if (!id)
		id = "";

	svc = svc_find(cmd, id);
	if (!svc) {
		_d("Creating new svc for %s id #%s type %d", cmd, id, type);
		svc = svc_new(cmd, id, type);
		if (!svc) {
			_e("Out of memory, cannot register service %s", cmd);
			return errno = ENOMEM;
		}

		if (type == SVC_TYPE_SERVICE && manual)
			svc_stop(svc);
	} else {
		/* e.g., if missing cmd or env before */
		svc_unblock(svc);
	}

	/* Always clear svc PID file, for now.  See TODO */
	svc->pidfile[0] = 0;
	/* Decode any optional pid:/optional/path/to/file.pid */
	if (pid && svc_is_daemon(svc) && pid_file_parse(svc, pid))
		_e("Invalid 'pid' argument to service: %s", pid);

	if (username) {
		char *ptr = strchr(username, ':');

		if (ptr) {
			*ptr++ = 0;
			strlcpy(svc->group, ptr, sizeof(svc->group));
		}
		strlcpy(svc->username, username, sizeof(svc->username));
	} else {
		getcuser(svc->username, sizeof(svc->username));
		getcgroup(svc->group, sizeof(svc->group));
	}

	parse_cmdline_args(svc, cmd);

	svc->runlevels = levels;
	_d("Service %s runlevel 0x%02x", svc->cmd, svc->runlevels);

	conf_parse_cond(svc, cond);

	parse_name(svc, name);
	if (halt)
		parse_sighalt(svc, halt);
	if (delay)
		parse_killdelay(svc, delay);
	if (log)
		parse_log(svc, log);
	if (desc)
		strlcpy(svc->desc, desc, sizeof(svc->desc));
	if (env)
		parse_env(svc, env);
	if (file)
		strlcpy(svc->file, file, sizeof(svc->file));

	/* Set configured limits */
	memcpy(svc->rlimit, rlimit, sizeof(svc->rlimit));

	/* Seed with currently active group, may be empty */
	strlcpy(svc->cgroup.name, cgroup_current, sizeof(svc->cgroup.name));
	if (cgroup)
		parse_cgroup(svc, cgroup);

	/* New, recently modified or unchanged ... used on reload. */
	if ((file && conf_changed(file)) || conf_changed(svc_getenv(svc)))
		svc_mark_dirty(svc);
	else
		svc_mark_clean(svc);

	svc_enable(svc);

	/* for finit native services only, e.g. plugins/hotplug.c */
	if (!file)
		svc->protect = 1;

	return 0;
}

/*
 * This function is called when cleaning up lingering (stopped) services
 * after a .conf reload.
 *
 * We need to ensure we properly stop the service before removing it,
 * including stopping any pending restart or SIGKILL timers before we
 * proceed to free() the svc itself.
 */
void service_unregister(svc_t *svc)
{
	if (!svc)
		return;

	service_stop(svc);
	svc_del(svc);
}

void service_monitor(pid_t lost, int status)
{
	svc_t *svc;

	if (fexist(SYNC_SHUTDOWN) || lost <= 1)
		return;

	if (tty_respawn(lost))
		return;

	svc = svc_find_by_pid(lost);
	if (!svc) {
		_d("collected unknown PID %d", lost);
		return;
	}

	_d("collected %s(%d), normal exit: %d, signaled: %d, exit code: %d",
	   svc->cmd, lost, WIFEXITED(status), WIFSIGNALED(status), WEXITSTATUS(status));
	svc->status = status;

	/* Forking sysv/services declare themselves with pid:!/path/to/pid.file  */
	if (svc_is_starting(svc) && svc_is_forking(svc))
		return;

	/* Try removing PID file (in case service does not clean up after itself) */
	if (svc_is_daemon(svc)) {
		service_cleanup(svc);
	} else if (svc_is_runtask(svc)) {
		/* run/task should run at least once per runlevel */
		if (WIFEXITED(status) && !WEXITSTATUS(status))
			svc->started = 1;
		else
			svc->started = 0;
	}

	/* Terminate any children in the same proess group, e.g. logit */
	kill(-svc->pid, SIGKILL);

	/* No longer running, update books. */
	svc->start_time = svc->pid = 0;

	if (!service_step(svc)) {
		/* Clean out any bootstrap tasks, they've had their time in the sun. */
		if (svc_clean_bootstrap(svc))
			_d("collected bootstrap task %s(%d), removing.", svc->cmd, lost);
	}

	sm_step(&sm);
}

static void svc_mark_affected(char *cond)
{
	svc_t *svc, *iter = NULL;

	for (svc = svc_iterator(&iter, 1); svc; svc = svc_iterator(&iter, 0)) {
		if (!svc_has_cond(svc))
			continue;

		if (cond_affects(cond, svc->cond))
			svc_mark_dirty(svc);
	}
}

/*
 * Called on conf_reload() to update service reverse dependencies.
 * E.g., if ospfd depends on zebra and the zebra Finit conf has
 * changed, we need to mark the ospfd Finit conf as changed too.
 */
void service_update_rdeps(void)
{
	svc_t *svc, *iter = NULL;

	for (svc = svc_iterator(&iter, 1); svc; svc = svc_iterator(&iter, 0)) {
		char cond[MAX_COND_LEN];

		if (!svc_is_changed(svc))
			continue;

		svc_mark_affected(mkcond(svc, cond, sizeof(cond)));
	}
}

static void service_retry(svc_t *svc)
{
	int timeout;
	char *restart_cnt = (char *)&svc->restart_cnt;

	service_timeout_cancel(svc);

	if (svc->state != SVC_HALTED_STATE ||
	    svc->block != SVC_BLOCK_RESTARTING) {
		_d("%s not crashing anymore", svc->cmd);
		*restart_cnt = 0;
		return;
	}

	if (*restart_cnt >= SVC_RESPAWN_MAX) {
		logit(LOG_CONSOLE | LOG_WARNING, "Service %s keeps crashing, not restarting.",
		      svc_ident(svc, NULL, 0));
		svc_crashing(svc);
		*restart_cnt = 0;
		service_step(svc);
		return;
	}

	(*restart_cnt)++;

	_d("%s crashed, trying to start it again, attempt %d", svc->cmd, *restart_cnt);
	logit(LOG_CONSOLE | LOG_WARNING, "Service %s[%d] died, restarting (%d/%d)",
	      svc_ident(svc, NULL, 0), svc->oldpid, *restart_cnt, SVC_RESPAWN_MAX);
	svc_unblock(svc);
	service_step(svc);

	/* Wait 2s for the first 5 respawns, then back off to 5s */
	timeout = ((*restart_cnt) <= (SVC_RESPAWN_MAX / 2)) ? 2000 : 5000;
	service_timeout_after(svc, timeout, service_retry);
}

static void svc_set_state(svc_t *svc, svc_state_t new)
{
	svc_state_t *state = (svc_state_t *)&svc->state;

	*state = new;

	/* if PID isn't collected within SVC_TERM_TIMEOUT msec, kill it! */
	if ((*state == SVC_STOPPING_STATE)) {
		_d("%s is stopping, wait %d sec before sending SIGKILL ...",
		   svc->cmd, svc->killdelay / 1000);
		service_timeout_cancel(svc);
		service_timeout_after(svc, svc->killdelay, service_kill);
	}
}

/*
 * Transition task/run/service
 *
 * Returns: non-zero if the @svc is no longer valid (removed)
 */
int service_step(svc_t *svc)
{
	cond_state_t cond;
	svc_state_t old_state;
	svc_cmd_t enabled;
	char *restart_cnt = (char *)&svc->restart_cnt;
	int changed = 0;
	int err;

restart:
	old_state = svc->state;
	enabled = svc_enabled(svc);

	_d("%20s(%4d): %8s %3sabled/%-7s cond:%-4s", svc->cmd, svc->pid,
	   svc_status(svc), enabled ? "en" : "dis", svc_dirtystr(svc),
	   condstr(cond_get_agg(svc->cond)));

	switch (svc->state) {
	case SVC_HALTED_STATE:
		if (enabled)
			svc_set_state(svc, SVC_READY_STATE);
		break;

	case SVC_DONE_STATE:
		if (svc_is_changed(svc))
			svc_set_state(svc, SVC_HALTED_STATE);
		break;

	case SVC_STOPPING_STATE:
		if (!svc->pid) {
			char cond[MAX_COND_LEN];

			/* PID was collected normally, no need to kill it */
			_d("%s: stopped normally, no need to send SIGKILL :)", svc->cmd);
			service_timeout_cancel(svc);

			_d("%s: clearing pid condition ...", svc->name);
			cond_clear(mkcond(svc, cond, sizeof(cond)));

			switch (svc->type) {
			case SVC_TYPE_SERVICE:
				svc_set_state(svc, SVC_HALTED_STATE);
				break;

			case SVC_TYPE_TASK:
			case SVC_TYPE_RUN:
			case SVC_TYPE_SYSV:
				svc_set_state(svc, SVC_DONE_STATE);
				break;

			default:
				_e("unknown service type %d", svc->type);
				break;
			}
		}
		break;

	case SVC_READY_STATE:
		if (!enabled) {
			svc_set_state(svc, SVC_HALTED_STATE);
		} else if (cond_get_agg(svc->cond) == COND_ON) {
			/* wait until all processes have been stopped before continuing... */
			if (sm_is_in_teardown(&sm))
				break;

			err = service_start(svc);
			if (err) {
				if (svc_is_missing(svc)) {
					svc_set_state(svc, SVC_HALTED_STATE);
					break;
				}
				(*restart_cnt)++;
				break;
			}

			/* Everything went fine, clean and set state */
			svc_mark_clean(svc);
			svc_set_state(svc, SVC_RUNNING_STATE);
		}
		break;

	case SVC_RUNNING_STATE:
		if (!enabled) {
			service_stop(svc);
			break;
		}

		if (!svc->pid) {
			if (svc_is_daemon(svc)) {
				svc_restarting(svc);
				svc_set_state(svc, SVC_HALTED_STATE);

				/*
				 * Restart directly after the first crash,
				 * then retry after 2 sec
				 */
				_d("delayed restart of %s", svc->cmd);
				service_timeout_after(svc, 1, service_retry);
				break;
			}

			if (svc_is_runtask(svc)) {
				if (svc_is_sysv(svc)) {
					if (!svc->started)
						svc_set_state(svc, SVC_STOPPING_STATE);
				} else {
					svc_set_state(svc, SVC_STOPPING_STATE);
				}
				svc->once++;
				break;
			}
		}

		cond = cond_get_agg(svc->cond);
		switch (cond) {
		case COND_OFF:
			service_stop(svc);
			break;

		case COND_FLUX:
			kill(svc->pid, SIGSTOP);
			svc_set_state(svc, SVC_WAITING_STATE);
			break;

		case COND_ON:
			if (svc_is_changed(svc)) {
				if (svc_nohup(svc))
					service_stop(svc);
				else {
					/*
					 * wait until all processes have been
					 * stopped before continuing...
					 */
					if (sm_is_in_teardown(&sm))
						break;
					service_restart(svc);
				}

				svc_mark_clean(svc);
			}
			break;
		}
		break;

	case SVC_WAITING_STATE:
		if (!enabled) {
			kill(svc->pid, SIGCONT);
			service_stop(svc);
			break;
		}

		if (!svc->pid) {
			(*restart_cnt)++;
			svc_set_state(svc, SVC_READY_STATE);
			break;
		}

		cond = cond_get_agg(svc->cond);
		switch (cond) {
		case COND_ON:
			kill(svc->pid, SIGCONT);
			svc_set_state(svc, SVC_RUNNING_STATE);
			/* Reassert condition if we go from waiting and no change */
			if (!svc_is_changed(svc)) {
				char name[MAX_COND_LEN];

				mkcond(svc, name, sizeof(name));
				_d("Reassert condition %s", name);
				cond_set_path(cond_path(name), COND_ON);
			}
			break;

		case COND_OFF:
			_d("Condition for %s is off, sending SIGCONT + SIGTERM", svc->name);
			kill(svc->pid, SIGCONT);
			service_stop(svc);
			break;

		case COND_FLUX:
			break;
		}
		break;
	}

	if (svc->state != old_state) {
		_d("%20s(%4d): -> %8s", svc->cmd, svc->pid, svc_status(svc));
		changed++;
		goto restart;
	}

	/*
	 * When a run/task/service changes state, e.g. transitioning from
	 * waiting to running, other services may need to change state too.
	 */
	if (changed)
		schedule_work(&work);

	return 0;
}

void service_step_all(int types)
{
	svc_foreach_type(types, service_step);
}

void service_worker(void *unused)
{
	service_step_all(SVC_TYPE_SERVICE | SVC_TYPE_RUNTASK);
}

/**
 * svc_clean_runtask - Clear once flag of runtasks
 *
 * XXX: runtasks should be stopped before calling this
 */
void service_runtask_clean(void)
{
	svc_t *svc, *iter = NULL;

	for (svc = svc_iterator(&iter, 1); svc; svc = svc_iterator(&iter, 0)) {
		if (!svc_is_runtask(svc))
			continue;

		svc->once = 0;
		if (svc->state == SVC_DONE_STATE)
			svc_set_state(svc, SVC_HALTED_STATE);
	}
}

/**
 * service_completed - Have run/task completed in current runlevel
 *
 * This function checks if all run/task have run once in the current
 * runlevel.  E.g., at bootstrap we must wait for these scripts or
 * programs to complete their run before switching to the configured
 * runlevel.
 *
 * All tasks with %HOOK_SVC_UP, %HOOK_SYSTEM_UP set in their condition
 * mask are skipped.  These tasks cannot run until finalize()
 *
 * Returns:
 * %TRUE(1) or %FALSE(0)
 */
int service_completed(void)
{
	svc_t *svc, *iter = NULL;

	for (svc = svc_iterator(&iter, 1); svc; svc = svc_iterator(&iter, 0)) {
		if (!svc_is_runtask(svc))
			continue;

		if (!svc_enabled(svc))
			continue;

		if (strstr(svc->cond, plugin_hook_str(HOOK_SVC_UP)) ||
		    strstr(svc->cond, plugin_hook_str(HOOK_SYSTEM_UP))) {
			_d("Skipping %s(%s), post-strap hook", svc->desc, svc->cmd);
			continue;
		}

		if (!svc->once) {
			_d("%s has not yet completed ...", svc->cmd);
			return 0;
		}
		_d("%s has completed ...", svc->cmd);
	}

	return 1;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
