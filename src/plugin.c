/* Plugin based services architecture for finit
 *
 * Copyright (c) 2012-2023  Joachim Wiberg <troglobit@gmail.com>
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

#include <errno.h>
#include <dlfcn.h>		/* dlopen() et al */
#include <dirent.h>		/* readdir() et al */
#include <poll.h>
#include <string.h>
#ifdef _LIBITE_LITE
# include <libite/lite.h>
# include <libite/queue.h>	/* BSD sys/queue.h API */
#else
# include <lite/lite.h>
# include <lite/queue.h>	/* BSD sys/queue.h API */
#endif

#include "cond.h"
#include "finit.h"
#include "helpers.h"
#include "plugin.h"
#include "private.h"
#include "service.h"
#include "sig.h"
#include "util.h"

#define is_io_plugin(p) ((p)->io.cb && (p)->io.fd > 0)
#define SEARCH_PLUGIN(str)						\
	PLUGIN_ITERATOR(p, tmp) {					\
		if (!strcmp(p->name, str))				\
			return p;					\
	}

static int plugloaded = 0;	/* Set while plugins are loaded */
static char *plugpath = NULL;	/* Set by first load. */
static TAILQ_HEAD(plugin_head, plugin) plugins  = TAILQ_HEAD_INITIALIZER(plugins);

#ifndef ENABLE_STATIC
static void check_plugin_depends(plugin_t *plugin);
#endif


static char *trim_ext(char *name)
{
	char *ptr;

	if (name) {
		ptr = strstr(name, ".so");
		if (!ptr)
			ptr = strstr(name, ".c");
		if (ptr)
			*ptr = 0;
	}

	return name;
}

int plugin_register(plugin_t *plugin)
{
	if (!plugin) {
		errno = EINVAL;
		return 1;
	}

	/* Setup default name if none is provided */
	if (!plugin->name) {
#ifndef ENABLE_STATIC
		Dl_info info;

		if (dladdr(plugin, &info) && info.dli_fname)
			plugin->name = basenm(info.dli_fname);
#endif
		if (!plugin->name)
			plugin->name = "unknown";
	}
	plugin->name = trim_ext(strdup(plugin->name));

	/* Already registered? */
	if (plugin_find(plugin->name)) {
		dbg("... %s already loaded", plugin->name);
		free(plugin->name);

		return 0;
	}

#ifndef ENABLE_STATIC
	/* Resolve plugin dependencies */
	check_plugin_depends(plugin);
#endif

	TAILQ_INSERT_TAIL(&plugins, plugin, link);

	return 0;
}

/* Not called, at the moment plugins cannot be unloaded. */
int plugin_unregister(plugin_t *plugin)
{
	if (is_io_plugin(plugin))
		uev_io_stop(&plugin->watcher);

#ifndef ENABLE_STATIC
	TAILQ_REMOVE(&plugins, plugin, link);

	dbg("%s exiting ...", plugin->name);
	free(plugin->name);
#else
	dbg("Finit built statically, cannot unload %s ...", plugin->name);
#endif

	return 0;
}

/**
 * plugin_find - Find a plugin by name
 * @name: With or without path, or .so extension
 *
 * This function uses an opporunistic search for a suitable plugin and
 * returns the first match.  Albeit with at least some measure of
 * heuristics.
 *
 * First it checks for an exact match.  If no match is found and @name
 * starts with a slash the search ends.  Otherwise a new search with the
 * plugin path prepended to @name is made.  Also, if @name does not end
 * with .so it too is added to @name before searching.
 *
 * Returns:
 * On success the pointer to the matching &plugin_t is returned,
 * otherwise %NULL is returned.
 */
plugin_t *plugin_find(char *name)
{
	plugin_t *p, *tmp;

	if (!name) {
		errno = EINVAL;
		return NULL;
	}

	SEARCH_PLUGIN(name);

	if (plugpath && name[0] != '/') {
		char path[CMD_SIZE];
		int noext;

		noext = strcmp(name + strlen(name) - 3, ".so");
		snprintf(path, sizeof(path), "%s%s%s%s", plugpath,
			 fisslashdir(plugpath) ? "" : "/",
			 name, noext ? ".so" : "");

		SEARCH_PLUGIN(path);
	}

	errno = ENOENT;
	return NULL;
}

/* Private daemon API *******************************************************/
#define CHOOSE(x, y) y
static const char *hook_cond[] = HOOK_TYPES;
#undef CHOOSE

const char *plugin_hook_str(hook_point_t no)
{
	return hook_cond[no];
}

int plugin_exists(hook_point_t no)
{
	plugin_t *p, *tmp;

	PLUGIN_ITERATOR(p, tmp) {
		if (p->hook[no].cb)
			return 1;
	}

	return 0;
}

#if defined(HAVE_HOOK_SCRIPTS_PLUGIN) && defined(PLUGIN_HOOK_SCRIPTS_PATH)
#define CHOOSE(x, y) y
static const char *hscript_paths[] = HOOK_TYPES;
#undef CHOOSE

void plugin_script_run(hook_point_t no)
{
	const char *hook_name = hscript_paths[no];
	const char *env[] = {
		"FINIT_HOOK_NAME", hook_name,
		"FINIT_SHUTDOWN", NULL,
		NULL,
	};
	char path[CMD_SIZE] = "";

	strlcat(path, PLUGIN_HOOK_SCRIPTS_PATH, sizeof(path));
	strlcat(path, hook_name + 4, sizeof(path));

	if (no >= HOOK_SHUTDOWN) {
		switch (halt) {
		case SHUT_OFF:
			env[3] = "poweroff";
			break;
		case SHUT_HALT:
			env[3] = "halt";
			break;
		case SHUT_REBOOT:
			env[3] = "reboot";
			break;
		}
	} else
		env[2] = NULL;

	run_parts(path, NULL, env, 0, 0);
}
#else
void plugin_script_run(hook_point_t no)
{
	(void)no;
}
#endif

/* Some hooks are called with a fixed argument */
void plugin_run_hook(hook_point_t no, void *arg)
{
	plugin_t *p, *tmp;

#ifdef HAVE_HOOK_SCRIPTS_PLUGIN
	if (!cond_is_available() && !plugloaded) {
		dbg("conditions not available, calling script based hooks only!");
		plugin_script_run(no);
	}
#endif

	PLUGIN_ITERATOR(p, tmp) {
		if (p->hook[no].cb) {
			dbg("Calling %s hook n:o %d (arg: %p) ...", basenm(p->name), no, arg ?: "NIL");
			p->hook[no].cb(arg ? arg : p->hook[no].arg);
		}
	}

	/*
	 * Conditions are stored in /run, so don't try to signal
	 * conditions for any hooks before filesystems have been
	 * mounted.
	 */
	if (cond_is_available() && no >= HOOK_BASEFS_UP && no <= HOOK_SHUTDOWN)
		cond_set_oneshot(hook_cond[no]);

	service_step_all(SVC_TYPE_RUNTASK);
}

/* Regular hooks are called with the registered plugin's argument */
void plugin_run_hooks(hook_point_t no)
{
	plugin_run_hook(no, NULL);
}

/*
 * Generic libev I/O callback, looks up correct plugin and calls its
 * callback.  libuEv might return UEV_ERROR in events, it is up to the
 * plugin callback to handle this.
 */
static void generic_io_cb(uev_t *w, void *arg, int events)
{
	plugin_t *p = (plugin_t *)arg;

	if (is_io_plugin(p) && p->io.fd == w->fd) {
		/* Stop watcher, callback may close descriptor on us ... */
		uev_io_stop(w);

//		dbg("Calling I/O %s from runloop...", basename(p->name));
		p->io.cb(p->io.arg, w->fd, events);

		/* Update fd, may be changed by plugin callback, e.g., if FIFO */
		uev_io_set(w, p->io.fd, p->io.flags);
	}
}

int plugin_io_init(plugin_t *p)
{
	if (!is_io_plugin(p))
		return 0;

	dbg("Initializing plugin %s for I/O", basenm(p->name));
	if (uev_io_init(ctx, &p->watcher, generic_io_cb, p, p->io.fd, p->io.flags)) {
		warn("Failed setting up I/O plugin %s", basenm(p->name));
		return 1;
	}

	return 0;
}

/* Setup any I/O callbacks for plugins that use them */
static int init_plugins(uev_ctx_t *ctx)
{
	plugin_t *p, *tmp;
	int fail = 0;

	PLUGIN_ITERATOR(p, tmp) {
		if (plugin_io_init(p))
			fail++;
	}

	return fail;
}

#ifndef ENABLE_STATIC
/**
 * load_one - Load one plugin
 * @path: Path to finit plugins, usually %PLUGIN_PATH
 * @name: Name of plugin, optionally ending in ".so"
 *
 * Loads a plugin from @path/@name[.so].  Note, if ".so" is missing from
 * the plugin @name it is added before attempting to load.
 *
 * It is up to the plugin itself or register itself as a "ctor" with the
 * %PLUGIN_INIT macro so that plugin_register() is called automatically.
 *
 * Returns:
 * POSIX OK(0) on success, non-zero otherwise.
 */
static int load_one(char *path, char *name)
{
	int noext;
	char sofile[CMD_SIZE];
	void *handle;
	plugin_t *plugin;

	if (!path || !fisdir(path) || !name) {
		errno = EINVAL;
		return 1;
	}

	/* Compose full path, with optional .so extension, to plugin */
	noext = strcmp(name + strlen(name) - 3, ".so");
	snprintf(sofile, sizeof(sofile), "%s/%s%s", path, name, noext ? ".so" : "");

	dbg("Loading plugin %s ...", sofile);
	handle = dlopen(sofile, RTLD_LAZY | RTLD_LOCAL);
	if (!handle) {
		char *error = dlerror();

		warn("Failed loading plugin %s: %s", sofile, error ? error : "unknown error");
		return 1;
	}

	plugin = TAILQ_LAST(&plugins, plugin_head);
	if (!plugin) {
		warn("Plugin %s failed to register, unloading from memory", sofile);
		dlclose(handle);
		return 1;
	}

	/* Remember handle from dlopen() for plugin_unregister() */
	plugin->handle = handle;

	return 0;
}

/**
 * check_plugin_depends - Check and load any plugins this one depends on.
 * @plugin: Plugin with possible depends to check
 *
 * Very simple dependency resolver, should actually load the plugin of
 * the correct .name, but currently loads a matching filename.
 *
 * Works, but only for now.  A better way might be to have a try_load()
 * that actually loads all plugins and checks their &plugin_t for the
 * correct .name.
 */
static void check_plugin_depends(plugin_t *plugin)
{
	int i;

	for (i = 0; i < PLUGIN_DEP_MAX && plugin->depends[i]; i++) {
//		dbg("Plugin %s depends on %s ...", plugin->name, plugin->depends[i]);
		if (plugin_find(plugin->depends[i])) {
//			dbg("OK plugin %s was already loaded.", plugin->depends[i]);
			continue;
		}

		load_one(plugpath, plugin->depends[i]);
	}
}

static int load_plugins(char *path)
{
	struct dirent *entry;
	int fail = 0;
	DIR *dp;

	dp = opendir(path);
	if (!dp) {
		if (errno == ENOENT)
			return 0;
		warn("Failed, cannot open plugin directory %s: %s", path, strerror(errno));
		return 1;
	}
	plugpath = path;

	while ((entry = readdir(dp))) {
		if (entry->d_name[0] == '.')
			continue; /* Skip . and .. directories */

		if (load_one(path, entry->d_name))
			fail++;
	}

	closedir(dp);

	return fail;
}
#else
static int load_plugins(char *path)
{
	print_desc("Initializing plugins", NULL);
	return 0;
}
#endif	/* ENABLE_STATIC */

int plugin_list(char *buf, size_t len)
{
#ifndef ENABLE_STATIC
	plugin_t *p, *tmp;

	buf[0] = 0;
	PLUGIN_ITERATOR(p, tmp) {
		if (buf[0])
			strlcat(buf, " ", len);
		strlcat(buf, p->name, len);
	}
#else
	buf[0] = 0;
#endif
	return 0;
}

int plugin_deps(char *buf, size_t len)
{
#ifndef ENABLE_STATIC
	plugin_t *p;

	p = plugin_find(buf);
	buf[0] = 0;
	if (p) {
		int i;

		for (i = 0; i < PLUGIN_DEP_MAX; i++) {
			if (!p->depends[i])
				continue;

			if (buf[0])
				strlcat(buf, " ", len);
			strlcat(buf, p->depends[i], len);
		}
	}
#else
	buf[0] = 0;
#endif
	return 0;
}

int plugin_init(uev_ctx_t *ctx)
{
	load_plugins(PLUGIN_PATH);

#ifdef EXTERNAL_PLUGIN_PATH
	char *paths = strdup(EXTERNAL_PLUGIN_PATH);
	char *path;

	if (paths) {
		dbg("Loading external plugins from %s ...", paths);
		path = strtok(paths, ":");
		while (path) {
			load_plugins(path);
			path = strtok(NULL, ":");
		}
		free(paths);
	}

	plugloaded = 1;
#endif

	return init_plugins(ctx);
}

void plugin_exit(void)
{
#ifndef ENABLE_STATIC
	plugin_t *p, *tmp;

        PLUGIN_ITERATOR(p, tmp) {
                if (dlclose(p->handle)) {
			char *error = dlerror();
                        warn("Failed unloading plugin %s: %s", p->name, error ? error : "unknown error");
		}
        }

	plugloaded = 0;
#endif
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
