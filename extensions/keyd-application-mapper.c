
#define _GNU_SOURCE

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_SECTIONS 256
#define MAX_BINDINGS 128
#define MAX_LINE 4096
#define MAX_CLASS 512
#define MAX_TITLE 4096

typedef struct {
	char *class_pat;
	char *title_pat;
	char *bindings[MAX_BINDINGS];
	int binding_count;
} Section;

static Section sections[MAX_SECTIONS];
static int section_count = 0;

static volatile sig_atomic_t running = 1;

static Display *dpy;
static Window root;

static Atom atom_active;
static Atom atom_net_wm_name;
static Atom atom_utf8_string;
static Atom atom_wm_class;

static char config_path[PATH_MAX];
static char lock_path[PATH_MAX];
static char log_path[PATH_MAX];

static time_t config_mtime = 0;

static bool verbose = false;

/* --------------------------------------------------------- */

static void die(const char *msg)
{
	fprintf(stderr, "ERROR: %s\n", msg);
	exit(EXIT_FAILURE);
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;

	char *end = s + strlen(s);

	while (end > s && isspace((unsigned char)end[-1]))
		--end;

	*end = '\0';

	return s;
}

static char *xstrdup(const char *s)
{
	char *p = strdup(s);

	if (!p)
		die("out of memory");

	return p;
}

static void free_config(void)
{
	for (int i = 0; i < section_count; i++) {
		free(sections[i].class_pat);
		free(sections[i].title_pat);

		for (int j = 0; j < sections[i].binding_count; j++)
			free(sections[i].bindings[j]);
	}

	section_count = 0;
}

/* --------------------------------------------------------- */

static void parse_section_header(char *line, Section **out)
{
	if (section_count >= MAX_SECTIONS)
		die("too many sections in app.conf");

	line[strlen(line) - 1] = '\0';
	char *inside = trim(line + 1);

	char *sep = strchr(inside, '|');

	Section *s = &sections[section_count++];

	if (sep) {
		*sep = '\0';

		s->class_pat = xstrdup(trim(inside));
		s->title_pat = xstrdup(trim(sep + 1));
	} else {
		s->class_pat = xstrdup(inside);
		s->title_pat = xstrdup("*");
	}

	s->binding_count = 0;

	*out = s;
}

static void parse_config(void)
{
	FILE *f = fopen(config_path, "r");

	if (!f)
		die("could not open app.conf");

	free_config();

	Section *current = NULL;
	char line[MAX_LINE];

	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line);

		if (*s == '\0' || *s == '#')
			continue;

		if (s[0] == '[' && s[strlen(s) - 1] == ']') {
			parse_section_header(s, &current);
			continue;
		}

		if (!current)
			continue;

		if (current->binding_count >= MAX_BINDINGS)
			die("too many bindings in a section");

		current->bindings[current->binding_count++] = xstrdup(s);
	}

	fclose(f);

	struct stat st;

	if (stat(config_path, &st) == 0)
		config_mtime = st.st_mtime;
}

/* --------------------------------------------------------- */

static void normalize_class(const char *src, char *dst, size_t size)
{
	size_t j = 0;

	for (size_t i = 0; src[i] && j + 1 < size; i++) {
		unsigned char c = (unsigned char)src[i];

		if (isalnum(c))
			dst[j++] = (char)tolower(c);
		else if (j > 0 && dst[j - 1] != '-')
			dst[j++] = '-';
	}

	while (j > 0 && dst[j - 1] == '-')
		j--;

	dst[j] = '\0';
}

static void normalize_title(const char *src, char *dst, size_t size)
{
	size_t j = 0;

	for (size_t i = 0; src[i] && j + 1 < size; i++) {
		unsigned char c = (unsigned char)src[i];

		if (isalnum(c))
			dst[j++] = (char)tolower(c);
		else if (j > 0 && dst[j - 1] != '-')
			dst[j++] = '-';
	}

	while (j > 0 && dst[j - 1] == '-')
		j--;

	dst[j] = '\0';
}

/* --------------------------------------------------------- */

static int lookup_bindings(const char *class_name, const char *title, char **out)
{
	int count = 0;

	for (int i = 0; i < section_count; i++) {
		Section *s = &sections[i];

		if (fnmatch(s->class_pat, class_name, 0) != 0)
			continue;

		if (fnmatch(s->title_pat, title, 0) != 0)
			continue;

		for (int j = 0; j < s->binding_count; j++) {
			if (count >= MAX_BINDINGS)
				break;

			/*
			 * Python implementation simply extends the list.
			 * We preserve that behaviour, including duplicate
			 * bindings.
			 */
			out[count++] = s->bindings[j];
		}
	}

	return count;
}

/* --------------------------------------------------------- */

static int run_keyd(char **bindings, int count)
{
	char **argv = calloc(count + 4, sizeof(char *));
	if (!argv)
		die("out of memory");

	argv[0] = "keyd";
	argv[1] = "bind";
	argv[2] = "reset";

	for (int i = 0; i < count; i++)
		argv[i + 3] = bindings[i];

	argv[count + 3] = NULL;

	if (verbose) {
		fprintf(stderr, "Executing:");
		for (int i = 0; argv[i]; i++)
			fprintf(stderr, " <%s>", argv[i]);
		fprintf(stderr, "\n");
	}

	pid_t pid = fork();

	if (pid == -1) {
		perror("fork");
		free(argv);
		return -1;
	}

	if (pid == 0) {
		execvp("keyd", argv);

		perror("execvp");
		_exit(127);
	}

	int status;

	if (waitpid(pid, &status, 0) == -1) {
		perror("waitpid");
		free(argv);
		return -1;
	}

	free(argv);

	if (!WIFEXITED(status))
		return -1;

	return WEXITSTATUS(status);
}

/* --------------------------------------------------------- */

static bool get_property(Window win, Atom property, Atom requested_type, char **result)
{
	Atom actual_type;
	int actual_format;
	unsigned long nitems;
	unsigned long bytes_after;

	unsigned char *data = NULL;

	int rc = XGetWindowProperty(dpy, win, property, 0, 16384, False, requested_type, &actual_type, &actual_format, &nitems, &bytes_after, &data);

	if (rc != Success || !data)
		return false;

	if (actual_format != 8) {
		XFree(data);
		return false;
	}

	*result = xstrdup((char *)data);

	XFree(data);

	return true;
}

static void get_window_title(Window win, char *title, size_t size)
{
	title[0] = '\0';

	char *tmp = NULL;

	if (get_property(win, atom_net_wm_name, atom_utf8_string, &tmp)) {
		snprintf(title, size, "%s", tmp);
		free(tmp);
		return;
	}

	char *name = NULL;

	if (get_property(win, XA_WM_NAME, AnyPropertyType, &name)) {
		snprintf(title, size, "%s", name);
		free(name);
	}
}

static void get_window_class(Window win, char *class_name, size_t size)
{
	class_name[0] = '\0';

	XClassHint hint;

	if (!XGetClassHint(dpy, win, &hint))
		return;

	if (hint.res_class)
		snprintf(class_name, size, "%s", hint.res_class);

	if (hint.res_name)
		XFree(hint.res_name);

	if (hint.res_class)
		XFree(hint.res_class);
}

/* --------------------------------------------------------- */

static Window get_active_window(void)
{
	Atom actual_type;
	int actual_format;
	unsigned long nitems;
	unsigned long bytes_after;

	unsigned char *data = NULL;

	int rc = XGetWindowProperty(dpy, root, atom_active, 0, 1, False, XA_WINDOW, &actual_type, &actual_format, &nitems, &bytes_after, &data);

	if (rc != Success || !data || nitems == 0) {
		if (data)
			XFree(data);

		return None;
	}

	Window win = *(Window *)data;

	XFree(data);

	return win;
}

/* --------------------------------------------------------- */

static void process_active_window(void)
{
	Window win = get_active_window();

	if (win == None)
		return;

	char raw_class[MAX_CLASS];
	char raw_title[MAX_TITLE];

	char class_name[MAX_CLASS];
	char title[MAX_TITLE];

	get_window_class(win, raw_class, sizeof(raw_class));
	get_window_title(win, raw_title, sizeof(raw_title));

	normalize_class(raw_class, class_name, sizeof(class_name));

	normalize_title(raw_title, title, sizeof(title));

	if (verbose) {
		printf("Active window: %s|%s\n", class_name, title);
		fflush(stdout);
	}

	char *bindings[MAX_BINDINGS];

	int count = lookup_bindings(class_name, title, bindings);

	if (run_keyd(bindings, count) != 0) {
		fprintf(stderr, "keyd bind failed\n");
	}
}

/* --------------------------------------------------------- */

static void reload_if_needed(void)
{
	struct stat st;

	if (stat(config_path, &st) != 0)
		return;

	if (st.st_mtime != config_mtime) {
		printf("%s: Updated, reloading config...\n", config_path);

		parse_config();

		fflush(stdout);
	}
}

/* --------------------------------------------------------- */

static void handle_signal(int sig)
{
	(void)sig;
	running = 0;
}

/* --------------------------------------------------------- */

static int acquire_lock(void)
{
	int fd = open(lock_path, O_CREAT | O_RDWR, 0600);

	if (fd < 0)
		return -1;

	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		close(fd);
		return -2;
	}

	return fd;
}

/* --------------------------------------------------------- */

static void daemonize_process(void)
{
	pid_t pid = fork();

	if (pid < 0)
		die("fork failed");

	if (pid > 0)
		exit(EXIT_SUCCESS);

	if (setsid() < 0)
		die("setsid failed");

	pid = fork();

	if (pid < 0)
		die("fork failed");

	if (pid > 0)
		exit(EXIT_SUCCESS);

	chdir("/");

	int fd = open(log_path, O_CREAT | O_WRONLY | O_APPEND, 0600);

	if (fd < 0)
		die("could not open log file");

	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);

	if (fd > STDERR_FILENO)
		close(fd);

	int nullfd = open("/dev/null", O_RDONLY);

	if (nullfd >= 0) {
		dup2(nullfd, STDIN_FILENO);

		if (nullfd > STDIN_FILENO)
			close(nullfd);
	}
}

/* --------------------------------------------------------- */

static void usage(const char *prog)
{
	printf("Usage: %s [-v] [-d]\n\n"
	       "  -v, --verbose    print active window\n"
	       "  -d, --daemonize  run in background\n",
	       prog);
}

/* --------------------------------------------------------- */

int main(int argc, char **argv)
{
	bool daemon = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
			verbose = true;
		} else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemonize") == 0) {
			daemon = true;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	const char *home = getenv("HOME");

	if (!home)
		die("HOME is not set");

	snprintf(config_path, sizeof(config_path), "%s/.config/keyd/app.conf", home);

	snprintf(lock_path, sizeof(lock_path), "%s/.config/keyd/app.lock", home);

	snprintf(log_path, sizeof(log_path), "%s/.config/keyd/app.log", home);

	struct stat st;

	if (stat(config_path, &st) != 0)
		die("could not find app.conf");

	parse_config();

	int lockfd = acquire_lock();

	if (lockfd == -1)
		die("could not create app.lock");

	if (lockfd == -2)
		die("only one instance may run at a time");

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	dpy = XOpenDisplay(NULL);

	if (!dpy)
		die("could not open X display");

	root = DefaultRootWindow(dpy);

	atom_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);

	atom_net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);

	atom_utf8_string = XInternAtom(dpy, "UTF8_STRING", False);

	atom_wm_class = XInternAtom(dpy, "WM_CLASS", False);

	(void)atom_wm_class;

	/*
	 * We only care about _NET_ACTIVE_WINDOW changes.
	 */
	XSelectInput(dpy, root, PropertyChangeMask);

	XSync(dpy, False);

	if (daemon) {
		daemonize_process();
	}

	/*
	 * Apply mapping for the currently active window.
	 */
	process_active_window();

	while (running) {
		XEvent ev;

		XNextEvent(dpy, &ev);

		if (ev.type == PropertyNotify && ev.xproperty.window == root && ev.xproperty.atom == atom_active) {

			reload_if_needed();
			process_active_window();
		}
	}

	XCloseDisplay(dpy);
	close(lockfd);

	return 0;
}
