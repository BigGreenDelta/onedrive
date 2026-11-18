// Files On-Demand mount for the OneDrive Client for Linux (onedrive --mount-on-demand).
//
// A FUSE filesystem that passes every operation through to the sync directory (the
// "backing" directory). The only addition: when an application opens or truncates a
// Files On-Demand placeholder (a file carrying the user.onedrive.placeholder extended
// attribute), the bridge first runs
//
//     <onedrive> --confdir <confdir> --download-now <backing path>
//
// and waits for it, so the application gets the real content, as with Windows Files
// On-Demand. Everything else is local disk I/O: the mount never talks to the network,
// so a slow or throttled OneDrive cannot stall directory listings or reads of files that
// are already on this device.
//
// Hydration runs in a child process (the normal client binary) rather than in-process:
// FUSE worker threads never enter the D runtime, and the download reuses the client's
// tested download, integrity and promotion code unchanged.

#define _GNU_SOURCE
#define FUSE_USE_VERSION 31

#include <fuse.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

extern char **environ;

#define PLACEHOLDER_XATTR "user.onedrive.placeholder"

static char backing_root[PATH_MAX];
static char hydrate_exe[PATH_MAX];
static char conf_dir[PATH_MAX];

// At most this many downloads run at once. libfuse runs a limited number of worker
// threads; keeping downloads below that leaves threads free for listings and reads of
// files that are already on this device, so a burst of opens (a thumbnailer, a search)
// cannot freeze the whole mount.
#define MAX_PARALLEL_DOWNLOADS 4
// After a download fails, opens of the same file fail at once for this long instead of
// each waiting application retrying the full download (for example while offline).
#define FAILURE_MEMORY_SECONDS 30
// A download that makes no progress for this many seconds is stopped and the open fails
// with ETIMEDOUT. Set from 'on_demand_stall_timeout'.
static int stall_timeout_seconds = 60;

// Build the backing path for a FUSE path ("/a/b" -> "<backing_root>/a/b")
static int backing_path(char out[PATH_MAX], const char *path) {
	int n = snprintf(out, PATH_MAX, "%s%s", backing_root, path);
	return (n < 0 || n >= PATH_MAX) ? -ENAMETOOLONG : 0;
}

static int is_placeholder(const char *real) {
	struct stat st;
	if (lstat(real, &st) != 0 || !S_ISREG(st.st_mode)) {
		return 0;
	}
	return lgetxattr(real, PLACEHOLDER_XATTR, NULL, 0) >= 0;
}

// ----------------------------------------------------------------------------------
// Hydration: one download per path at a time; concurrent openers wait for it
// ----------------------------------------------------------------------------------

struct inflight {
	char path[PATH_MAX];
	struct inflight *next;
};

static pthread_mutex_t inflight_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t inflight_done = PTHREAD_COND_INITIALIZER;
static struct inflight *inflight_head = NULL;
static int downloads_running = 0;

struct failure {
	char path[PATH_MAX];
	time_t when;
	struct failure *next;
};
static struct failure *failure_head = NULL;

// Caller holds inflight_lock
static int recently_failed(const char *real) {
	time_t now = time(NULL);
	for (struct failure **p = &failure_head; *p;) {
		if (now - (*p)->when > FAILURE_MEMORY_SECONDS) {
			struct failure *old = *p;
			*p = old->next;
			free(old);
			continue;
		}
		if (strcmp((*p)->path, real) == 0) {
			return 1;
		}
		p = &(*p)->next;
	}
	return 0;
}

// Caller holds inflight_lock
static void remember_failure(const char *real) {
	struct failure *f = calloc(1, sizeof(*f));
	if (!f) {
		return;
	}
	snprintf(f->path, sizeof(f->path), "%s", real);
	f->when = time(NULL);
	f->next = failure_head;
	failure_head = f;
}

static int inflight_contains(const char *real) {
	for (struct inflight *i = inflight_head; i; i = i->next) {
		if (strcmp(i->path, real) == 0) {
			return 1;
		}
	}
	return 0;
}

static double monotonic_seconds(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// A client that was killed, by us or by anything else, cannot clean up its per-process
// database copy: remove it here so copies do not pile up in the config folder
static void remove_hydrator_db(pid_t pid) {
	static const char *suffixes[] = {"", "-wal", "-shm", "-journal"};
	for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
		char db[PATH_MAX + 64];
		int n = snprintf(db, sizeof(db), "%s/items-ondemand-%ld.sqlite3%s", conf_dir, (long)pid, suffixes[i]);
		if (n > 0 && (size_t)n < sizeof(db)) {
			unlink(db);
		}
	}
}

// Stop a download that stopped making progress: ask first (the client then removes its
// private database), force it after a grace period. A forced stop leaves the '.partial'
// file in place, so the next open of a large file resumes from it instead of starting over.
static void stop_hydrator(pid_t pid) {
	int status;
	kill(pid, SIGTERM);
	double deadline = monotonic_seconds() + 5.0;
	while (monotonic_seconds() < deadline) {
		if (waitpid(pid, &status, WNOHANG) == pid) {
			return;
		}
		struct timespec tick = {0, 100 * 1000 * 1000};
		nanosleep(&tick, NULL);
	}
	kill(pid, SIGKILL);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	remove_hydrator_db(pid);
}

static int run_hydrator(const char *real) {
	char *argv[] = {hydrate_exe, "--confdir", conf_dir, "--download-now", (char *)real, NULL};
	pid_t pid;
	// The helper runs unattended: give it no terminal input, so nothing it does can wait on a
	// keyboard (the application that opened the file is blocked until it finishes)
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	int rc = posix_spawn(&pid, hydrate_exe, &actions, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	if (rc != 0) {
		fprintf(stderr, "onedrive-ondemand: cannot start %s: %s\n", hydrate_exe, strerror(rc));
		return -EIO;
	}
	// Watch the download instead of waiting blindly: the client retries a lost connection
	// for up to a year, and the application that opened the file waits as long as we do.
	// The limit is on progress, not on total time: a large file on a slow but working
	// connection keeps growing its '.partial' file and is never stopped.
	char partial[PATH_MAX + 16];
	snprintf(partial, sizeof(partial), "%s.partial", real);
	off_t last_size = -2;
	double last_progress = monotonic_seconds();
	int status = 0;
	for (;;) {
		pid_t done = waitpid(pid, &status, WNOHANG);
		if (done == pid) {
			break;
		}
		if (done < 0 && errno != EINTR) {
			return -EIO;
		}
		struct stat st;
		off_t size = (lstat(partial, &st) == 0) ? st.st_size : -1;
		double now = monotonic_seconds();
		if (size != last_size) {
			last_size = size;
			last_progress = now;
		} else if (now - last_progress >= stall_timeout_seconds) {
			fprintf(stderr, "onedrive-ondemand: no download progress for %d seconds, stopping the download of %s\n", stall_timeout_seconds, real);
			stop_hydrator(pid);
			return -ETIMEDOUT;
		}
		struct timespec tick = {0, 250 * 1000 * 1000};
		nanosleep(&tick, NULL);
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "onedrive-ondemand: download failed for %s\n", real);
		remove_hydrator_db(pid);
		return -EIO;
	}
	return 0;
}

// ----------------------------------------------------------------------------------
// Status for desktop tools: <confdir>/ondemand-status.json holds the number of files
// waiting for or in a download through this mount. Replaced atomically on each change.
// ----------------------------------------------------------------------------------

struct wanted {
	char path[PATH_MAX];
	int refs;
	struct wanted *next;
};
static struct wanted *wanted_head = NULL;
static int wanted_count = 0;

static void status_path(char out[PATH_MAX + 32], const char *suffix) {
	snprintf(out, PATH_MAX + 32, "%s/ondemand-status.json%s", conf_dir, suffix);
}

// Caller holds inflight_lock (or no FUSE thread runs yet)
static void write_status(void) {
	char final_path[PATH_MAX + 32], tmp_path[PATH_MAX + 32];
	status_path(final_path, "");
	status_path(tmp_path, ".tmp");
	char stamp[32];
	time_t now = time(NULL);
	struct tm utc;
	gmtime_r(&now, &utc);
	strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
	int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		return;
	}
	char doc[256];
	int n = snprintf(doc, sizeof(doc),
		"{\n    \"application\": \"onedrive-ondemand\",\n    \"pid\": %ld,\n    \"downloads_pending\": %d,\n    \"updated_at\": \"%s\"\n}\n",
		(long)getpid(), wanted_count, stamp);
	int ok = n > 0 && (size_t)n < sizeof(doc) && write(fd, doc, (size_t)n) == n;
	close(fd);
	if (!ok || rename(tmp_path, final_path) != 0) {
		unlink(tmp_path);
	}
}

static void remove_status(void) {
	char path[PATH_MAX + 32];
	status_path(path, "");
	unlink(path);
	status_path(path, ".tmp");
	unlink(path);
}

// Caller holds inflight_lock. Counts distinct files: three openers of one file are one download.
static void want(const char *real) {
	for (struct wanted *w = wanted_head; w; w = w->next) {
		if (strcmp(w->path, real) == 0) {
			w->refs++;
			return;
		}
	}
	struct wanted *w = calloc(1, sizeof(*w));
	if (!w) {
		return;
	}
	snprintf(w->path, sizeof(w->path), "%s", real);
	w->refs = 1;
	w->next = wanted_head;
	wanted_head = w;
	wanted_count++;
	write_status();
}

// Caller holds inflight_lock
static void unwant(const char *real) {
	for (struct wanted **p = &wanted_head; *p; p = &(*p)->next) {
		if (strcmp((*p)->path, real) == 0) {
			if (--(*p)->refs == 0) {
				struct wanted *old = *p;
				*p = old->next;
				free(old);
				wanted_count--;
				write_status();
			}
			return;
		}
	}
}

// Caller holds inflight_lock; returns with it held (it is released during the download)
static int hydrate_locked(const char *real) {
	while (inflight_contains(real) || downloads_running >= MAX_PARALLEL_DOWNLOADS) {
		pthread_cond_wait(&inflight_done, &inflight_lock);
	}
	// Another opener may have finished the download while we waited
	if (!is_placeholder(real)) {
		return 0;
	}
	if (recently_failed(real)) {
		return -EIO;
	}
	struct inflight *mine = calloc(1, sizeof(*mine));
	if (!mine) {
		return -ENOMEM;
	}
	downloads_running++;
	snprintf(mine->path, sizeof(mine->path), "%s", real);
	mine->next = inflight_head;
	inflight_head = mine;
	pthread_mutex_unlock(&inflight_lock);

	int rc = run_hydrator(real);
	if (rc == 0 && is_placeholder(real)) {
		rc = -EIO;
	}

	pthread_mutex_lock(&inflight_lock);
	downloads_running--;
	if (rc != 0) {
		remember_failure(real);
	}
	for (struct inflight **p = &inflight_head; *p; p = &(*p)->next) {
		if (*p == mine) {
			*p = mine->next;
			break;
		}
	}
	free(mine);
	pthread_cond_broadcast(&inflight_done);
	return rc;
}

// Programs that open files only to look at them: thumbnailers and desktop indexers. Opening an
// online-only file downloads all of it, so a file manager drawing thumbnails for a photo folder
// would download the whole folder (Windows never downloads online-only files for that). They are
// refused and show a generic icon. The file manager itself reads files only to tell their type,
// one after another, and each download costs seconds (a client process per file), so it is
// refused too: an online-only file downloads when it is opened from the file manager, which
// starts the application that reads it.

// Read /proc/<id>/comm into 'name'
static int read_comm(long id, char *name, size_t size) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/%ld/comm", id);
	FILE *f = fopen(path, "re");
	if (f == NULL) {
		return -1;
	}
	if (fgets(name, (int)size, f) == NULL) {
		fclose(f);
		return -1;
	}
	fclose(f);
	name[strcspn(name, "\n")] = '\0';
	return 0;
}

// The opener's thread name and its process name. FUSE reports the calling thread, and
// worker threads have their own names ("pool-org.gnome."), so both are needed.
static int opener_names(char *thread_name, char *process_name, size_t size) {
	struct fuse_context *ctx = fuse_get_context();
	thread_name[0] = process_name[0] = '\0';
	if (ctx == NULL || ctx->pid <= 0) {
		return -1;
	}
	read_comm((long)ctx->pid, thread_name, size);
	long tgid = (long)ctx->pid;
	char path[64];
	snprintf(path, sizeof(path), "/proc/%ld/status", (long)ctx->pid);
	FILE *f = fopen(path, "re");
	if (f != NULL) {
		char line[256];
		while (fgets(line, sizeof(line), f) != NULL) {
			if (strncmp(line, "Tgid:", 5) == 0) {
				tgid = strtol(line + 5, NULL, 10);
				break;
			}
		}
		fclose(f);
	}
	read_comm(tgid, process_name, size);
	return 0;
}

static int looks_only(const char *name) {
	// comm is cut to 15 characters: "glycin-thumbnai", "gsf-office-thum"
	return strncmp(name, "glycin", 6) == 0 || strstr(name, "thumb") != NULL
	    || strncmp(name, "localsearch", 11) == 0 || strncmp(name, "tracker", 7) == 0;
}

static int download_refused_for_opener(const char *real) {
	char thread_name[32], process_name[32];
	if (opener_names(thread_name, process_name, sizeof(thread_name)) != 0) {
		return 0;
	}
	if (looks_only(process_name) || looks_only(thread_name)) {
		fprintf(stderr, "onedrive-ondemand: not downloading %s for %s/%s (it only looks at files)\n", real, process_name, thread_name);
		return 1;
	}
	if (strcmp(process_name, "nautilus") == 0) {
		fprintf(stderr, "onedrive-ondemand: not downloading %s for the file manager (it downloads when opened)\n", real);
		return 1;
	}
	fprintf(stderr, "onedrive-ondemand: downloading %s for %s/%s\n", real, process_name, thread_name);
	return 0;
}

// Make sure the backing file at 'real' holds its content. 0 on success, -errno on failure.
static int ensure_hydrated(const char *real) {
	if (!is_placeholder(real)) {
		return 0;
	}
	if (download_refused_for_opener(real)) {
		return -EIO;
	}
	pthread_mutex_lock(&inflight_lock);
	want(real);
	int rc = hydrate_locked(real);
	unwant(real);
	pthread_mutex_unlock(&inflight_lock);
	return rc;
}

// ----------------------------------------------------------------------------------
// Passthrough operations
// ----------------------------------------------------------------------------------

static void *od_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
	(void)conn;
	cfg->use_ino = 1;
	// Unlink files immediately even while open: otherwise libfuse renames them to
	// '.fuse_hidden*' inside the sync directory, and the sync would upload that rename
	cfg->hard_remove = 1;
	// The sync engine changes backing files behind this mount: keep caches short and
	// let the kernel revalidate file data against the backing file's mtime and size
	cfg->entry_timeout = 1;
	cfg->attr_timeout = 1;
	cfg->negative_timeout = 0;
	cfg->auto_cache = 1;
	return NULL;
}

static int od_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
	int rc;
	if (fi) {
		rc = fstat((int)fi->fh, st);
	} else {
		char real[PATH_MAX];
		if ((rc = backing_path(real, path)) != 0) {
			return rc;
		}
		rc = lstat(real, st);
		// Placeholders are read-only on disk so nothing writes into their missing content.
		// Through the mount they look like normal files: opening one for writing downloads it first.
		if (rc == 0 && S_ISREG(st->st_mode) && lgetxattr(real, PLACEHOLDER_XATTR, NULL, 0) >= 0) {
			st->st_mode |= S_IRUSR | S_IWUSR;
		}
	}
	return rc == 0 ? 0 : -errno;
}

static int od_access(const char *path, int mask) {
	char real[PATH_MAX];
	int rc = backing_path(real, path);
	if (rc != 0) {
		return rc;
	}
	// A placeholder is writable through the mount (it is downloaded on open)
	if ((mask & W_OK) && is_placeholder(real)) {
		mask &= ~W_OK;
	}
	return access(real, mask) == 0 ? 0 : -errno;
}

static int od_readlink(const char *path, char *buf, size_t size) {
	char real[PATH_MAX];
	int rc = backing_path(real, path);
	if (rc != 0) {
		return rc;
	}
	ssize_t n = readlink(real, buf, size - 1);
	if (n < 0) {
		return -errno;
	}
	buf[n] = '\0';
	return 0;
}

static int od_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
	(void)offset;
	(void)fi;
	(void)flags;
	char real[PATH_MAX];
	int rc = backing_path(real, path);
	if (rc != 0) {
		return rc;
	}
	DIR *dir = opendir(real);
	if (!dir) {
		return -errno;
	}
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		struct stat st;
		memset(&st, 0, sizeof(st));
		st.st_ino = entry->d_ino;
		st.st_mode = (mode_t)DTTOIF(entry->d_type);
		if (filler(buf, entry->d_name, &st, 0, 0)) {
			break;
		}
	}
	closedir(dir);
	return 0;
}

static int od_mknod(const char *path, mode_t mode, dev_t rdev) {
	char real[PATH_MAX];
	int rc = backing_path(real, path);
	if (rc != 0) {
		return rc;
	}
	rc = S_ISFIFO(mode) ? mkfifo(real, mode) : mknod(real, mode, rdev);
	return rc == 0 ? 0 : -errno;
}

static int od_mkdir(const char *path, mode_t mode) {
	char real[PATH_MAX];
	int rc = backing_path(real, path);
	if (rc != 0) {
		return rc;
	}
	return mkdir(real, mode) == 0 ? 0 : -errno;
}

static int od_unlink(const char *path) {
	char real[PATH_MAX];
	int rc = backing_path(real, path);
	if (rc != 0) {
		return rc;
	}
	return unlink(real) == 0 ? 0 : -errno;
}

static int od_rmdir(const char *path) {
	char real[PATH_MAX];
	int rc = backing_path(real, path);
	if (rc != 0) {
		return rc;
	}
	return rmdir(real) == 0 ? 0 : -errno;
}

static int od_symlink(const char *target, const char *path) {
	char real[PATH_MAX];
	int rc = backing_path(real, path);
	if (rc != 0) {
		return rc;
	}
	return symlink(target, real) == 0 ? 0 : -errno;
}

static int od_rename(const char *from, const char *to, unsigned int flags) {
	char real_from[PATH_MAX], real_to[PATH_MAX];
	int rc = backing_path(real_from, from);
	if (rc == 0) {
		rc = backing_path(real_to, to);
	}
	if (rc != 0) {
		return rc;
	}
	if (flags & ~RENAME_NOREPLACE) {
		return -EINVAL;
	}
	if (flags & RENAME_NOREPLACE) {
		return renameat2(AT_FDCWD, real_from, AT_FDCWD, real_to, RENAME_NOREPLACE) == 0 ? 0 : -errno;
	}
	return rename(real_from, real_to) == 0 ? 0 : -errno;
}

static int od_link(const char *from, const char *to) {
	char real_from[PATH_MAX], real_to[PATH_MAX];
	int rc = backing_path(real_from, from);
	if (rc == 0) {
		rc = backing_path(real_to, to);
	}
	if (rc != 0) {
		return rc;
	}
	// A hard link to a placeholder would be a second placeholder no sync knows about
	if (is_placeholder(real_from)) {
		return -EPERM;
	}
	return link(real_from, real_to) == 0 ? 0 : -errno;
}

static int od_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
	if (fi) {
		return fchmod((int)fi->fh, mode) == 0 ? 0 : -errno;
	}
	char real[PATH_MAX];
	int rc = backing_path(real, path);
	if (rc != 0) {
		return rc;
	}
	// A placeholder stays read-only on disk (nothing may write into its missing content);
	// the requested mode is ignored. Download the file first to change its mode.
	if (is_placeholder(real)) {
		return 0;
	}
	return chmod(real, mode) == 0 ? 0 : -errno;
}

static int od_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
	if (fi) {
		return fchown((int)fi->fh, uid, gid) == 0 ? 0 : -errno;
	}
	char real[PATH_MAX];
	int rc = backing_path(real, path);
	if (rc != 0) {
		return rc;
	}
	return lchown(real, uid, gid) == 0 ? 0 : -errno;
}

static int od_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
	if (fi) {
		return ftruncate((int)fi->fh, size) == 0 ? 0 : -errno;
	}
	char real[PATH_MAX];
	int rc = backing_path(real, path);
	if (rc != 0) {
		return rc;
	}
	// Changing a placeholder's content means changing the real file: download it first
	if ((rc = ensure_hydrated(real)) != 0) {
		return rc;
	}
	return truncate(real, size) == 0 ? 0 : -errno;
}

static int od_utimens(const char *path, const struct timespec ts[2], struct fuse_file_info *fi) {
	if (fi) {
		return futimens((int)fi->fh, ts) == 0 ? 0 : -errno;
	}
	char real[PATH_MAX];
	int rc = backing_path(real, path);
	if (rc != 0) {
		return rc;
	}
	// A placeholder keeps the online modification time (the sync compares it); 'touch' is ignored
	if (is_placeholder(real)) {
		return 0;
	}
	return utimensat(AT_FDCWD, real, ts, AT_SYMLINK_NOFOLLOW) == 0 ? 0 : -errno;
}

static int od_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
	char real[PATH_MAX];
	int rc = backing_path(real, path);
	if (rc != 0) {
		return rc;
	}
	// O_CREAT on an existing placeholder opens that file: give it its content first
	if ((rc = ensure_hydrated(real)) != 0) {
		return rc;
	}
	int fd = open(real, fi->flags | O_CLOEXEC, mode);
	if (fd < 0) {
		return -errno;
	}
	fi->fh = (uint64_t)fd;
	return 0;
}

// Record an open as the file's access time. The monitor's automatic "free up space" makes the
// least recently opened files online-only first, and the sync folder is usually mounted noatime,
// so the kernel would not record it. The sync ignores access times (and the inotify watch does
// not report attribute changes), so this never reads as an edit. At most once an hour per file.
static void note_opened(int fd) {
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		return;
	}
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	if (st.st_atim.tv_sec > now.tv_sec - 3600) {
		return;
	}
	struct timespec times[2] = {{0, UTIME_NOW}, {0, UTIME_OMIT}};
	futimens(fd, times);
}

static int od_open(const char *path, struct fuse_file_info *fi) {
	char real[PATH_MAX];
	int rc = backing_path(real, path);
	if (rc != 0) {
		return rc;
	}
	// The one Files On-Demand step: an opened placeholder is downloaded before it is read
	if ((rc = ensure_hydrated(real)) != 0) {
		return rc;
	}
	int fd = open(real, fi->flags | O_CLOEXEC);
	if (fd < 0) {
		return -errno;
	}
	note_opened(fd);
	fi->fh = (uint64_t)fd;
	return 0;
}

static int od_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	(void)path;
	ssize_t n = pread((int)fi->fh, buf, size, offset);
	return n < 0 ? -errno : (int)n;
}

static int od_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	(void)path;
	ssize_t n = pwrite((int)fi->fh, buf, size, offset);
	return n < 0 ? -errno : (int)n;
}

static int od_statfs(const char *path, struct statvfs *st) {
	char real[PATH_MAX];
	int rc = backing_path(real, path);
	if (rc != 0) {
		return rc;
	}
	return statvfs(real, st) == 0 ? 0 : -errno;
}

static int od_flush(const char *path, struct fuse_file_info *fi) {
	(void)path;
	// Report close-time errors (as close() would) without closing the descriptor
	int dupfd = dup((int)fi->fh);
	if (dupfd < 0) {
		return -errno;
	}
	return close(dupfd) == 0 ? 0 : -errno;
}

static int od_release(const char *path, struct fuse_file_info *fi) {
	(void)path;
	close((int)fi->fh);
	return 0;
}

static int od_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
	(void)path;
	int rc = datasync ? fdatasync((int)fi->fh) : fsync((int)fi->fh);
	return rc == 0 ? 0 : -errno;
}

static int od_fallocate(const char *path, int mode, off_t offset, off_t length, struct fuse_file_info *fi) {
	(void)path;
	if (mode) {
		return -EOPNOTSUPP;
	}
	return -posix_fallocate((int)fi->fh, offset, length);
}

static off_t od_lseek(const char *path, off_t off, int whence, struct fuse_file_info *fi) {
	(void)path;
	off_t res = lseek((int)fi->fh, off, whence);
	return res < 0 ? -errno : res;
}

static const struct fuse_operations od_operations = {
	.init = od_init,
	.getattr = od_getattr,
	.access = od_access,
	.readlink = od_readlink,
	.readdir = od_readdir,
	.mknod = od_mknod,
	.mkdir = od_mkdir,
	.symlink = od_symlink,
	.unlink = od_unlink,
	.rmdir = od_rmdir,
	.rename = od_rename,
	.link = od_link,
	.chmod = od_chmod,
	.chown = od_chown,
	.truncate = od_truncate,
	.utimens = od_utimens,
	.open = od_open,
	.create = od_create,
	.read = od_read,
	.write = od_write,
	.statfs = od_statfs,
	.flush = od_flush,
	.release = od_release,
	.fsync = od_fsync,
	.fallocate = od_fallocate,
	.lseek = od_lseek,
};

// Entry point called from src/ondemand.d. Runs in the foreground until unmounted.
int onedrive_fuse_mount(const char *backing, const char *mountpoint, const char *exe, const char *confdir, int stall_timeout, int debug) {
	snprintf(backing_root, sizeof(backing_root), "%s", backing);
	stall_timeout_seconds = stall_timeout;
	// ONEDRIVE_ONDEMAND_HYDRATOR replaces the download command (same arguments). It exists for
	// tests of the mount that must run without a OneDrive account.
	const char *hydrator_override = getenv("ONEDRIVE_ONDEMAND_HYDRATOR");
	snprintf(hydrate_exe, sizeof(hydrate_exe), "%s", (hydrator_override && *hydrator_override) ? hydrator_override : exe);
	snprintf(conf_dir, sizeof(conf_dir), "%s", confdir);

	char *argv[8];
	int argc = 0;
	argv[argc++] = "onedrive-ondemand";
	argv[argc++] = (char *)mountpoint;
	argv[argc++] = "-f";
	argv[argc++] = "-o";
	argv[argc++] = "fsname=onedrive,subtype=onedrive";
	if (debug) {
		argv[argc++] = "-d";
	}
	argv[argc] = NULL;
	write_status();
	int rc = fuse_main(argc, argv, &od_operations, NULL);
	remove_status();
	return rc;
}
