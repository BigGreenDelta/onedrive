// What is this module called?
module ondemand;

// Files On-Demand: local placeholder primitives.
//
// A placeholder is a regular file that stands in for an online file whose content has
// not been downloaded. It has the online size (as a sparse file, so it uses no disk
// blocks), the online modification time, read-only permissions, and an extended
// attribute that records which online item it represents:
//
//     user.onedrive.placeholder = "<driveId>\n<itemId>\n<quickXorHash or sha256Hash>"
//
// (newline-separated: drive and item ids contain '!', e.g. "b!..." and "ABC!101").
//
// The PRESENCE of that attribute is the single source of truth for "this path holds no
// local content". Detection fails closed: an attribute that cannot be parsed still marks
// a placeholder, so the engine never treats its zeros as content. The sync engine never
// hashes, uploads or safeBackups a placeholder, and hydration replaces it atomically.
//
// A file downloaded on demand (outside a full sync cycle) carries
//
//     user.onedrive.hydrated = "<itemId>\n<content hash of the downloaded bytes>"
//
// so the engine can tell "the online bytes of this item, downloaded before the local
// database caught up" apart from "the user edited this file": only the latter uploads.
// The marker is bound to the item id, and cleared whenever the file is uploaded.
//
// Placeholder state is filesystem state, deliberately not a database column: adding a
// column would force every existing install through a full resync.

// What does this module require to function?
import core.stdc.errno;
import core.sys.posix.fcntl;
import core.sys.posix.unistd;
import core.sys.posix.sys.stat;
import core.sys.posix.time : timespec;
import std.algorithm : canFind, endsWith;
import std.array : split, empty;
import std.conv : to, octal;
import std.datetime : SysTime;
import std.file;
import std.path : absolutePath, buildNormalizedPath, expandTilde, baseName, dirName, buildPath;
import std.string : toStringz, fromStringz, join, startsWith, strip;
import std.uuid : randomUUID;
import core.stdc.stdlib : EXIT_SUCCESS, EXIT_FAILURE;

version (linux) {
	import core.sys.linux.sys.xattr;
	extern (C) int renameat2(int olddirfd, const(char)* oldpath, int newdirfd, const(char)* newpath, uint flags) nothrow @nogc;
	extern (C) int flock(int fd, int operation) nothrow @nogc;
	enum uint RENAME_NOREPLACE = 1;
	enum int LOCK_EX = 2;
	enum int F_SETLEASE = 1024;
}

// What other modules that we have created do we need to import?
import config;
import log;
import util;

enum string placeholderXattrName = "user.onedrive.placeholder";
enum string hydratedXattrName = "user.onedrive.hydrated";

// Placeholders are read-only so an application working on the sync directory directly
// cannot write into the empty content and turn it into a modification.
enum uint placeholderFileMode = octal!"400";

// The online identity a placeholder stands in for
struct PlaceholderIdentity {
	string driveId;
	string itemId;
	string contentHash;
}

// What a file looked like at one moment: used to prove it did not change while it was hashed
struct FileSnapshot {
	ulong inode;
	long size;
	long mtimeSec;
	long mtimeNsec;
	ulong links;
}

// Is Files On-Demand supported on this platform?
bool onDemandSupportedPlatform() {
	version (linux) {
		return true;
	} else {
		return false;
	}
}

private string encodeIdentity(PlaceholderIdentity identity) {
	return [identity.driveId, identity.itemId, identity.contentHash].join("\n");
}

private bool decodeIdentity(string value, out PlaceholderIdentity identity) {
	auto parts = value.split("\n");
	if (parts.length != 3 || parts[0].length == 0 || parts[1].length == 0) {
		return false;
	}
	identity = PlaceholderIdentity(parts[0], parts[1], parts[2]);
	return true;
}

private timespec toTimespec(SysTime time) {
	timespec ts;
	ts.tv_sec = cast(typeof(ts.tv_sec)) time.toUnixTime();
	ts.tv_nsec = cast(typeof(ts.tv_nsec)) time.fracSecs.total!"nsecs";
	return ts;
}

private string errnoText(string what) {
	return what ~ " failed (errno " ~ to!string(errno) ~ ")";
}

version (linux) {
	// Read one extended attribute of a path (not following a final symlink)
	private bool readXattr(string path, string attrName, out string value) {
		char[1024] buffer;
		ssize_t size = lgetxattr(toStringz(path), toStringz(attrName), buffer.ptr, buffer.length);
		if (size < 0) {
			return false;
		}
		value = buffer[0 .. size].idup;
		return true;
	}

	private bool hasXattr(string path, string attrName) {
		return lgetxattr(toStringz(path), toStringz(attrName), null, 0) >= 0;
	}
}

// Is this path a Files On-Demand placeholder? (fails closed: the attribute's presence decides)
bool isOnDemandPlaceholder(string path) {
	version (linux) {
		stat_t st;
		if (lstat(toStringz(path), &st) != 0 || !S_ISREG(st.st_mode)) {
			return false;
		}
		return hasXattr(path, placeholderXattrName);
	} else {
		return false;
	}
}

// Read the online identity a placeholder stands in for. False when the path is not a
// placeholder OR its marker cannot be parsed (isOnDemandPlaceholder() is still true then).
bool readPlaceholderIdentity(string path, out PlaceholderIdentity identity) {
	version (linux) {
		if (!isOnDemandPlaceholder(path)) {
			return false;
		}
		string value;
		if (!readXattr(path, placeholderXattrName, value)) {
			return false;
		}
		return decodeIdentity(value, identity);
	} else {
		return false;
	}
}

// Capture the identity-relevant state of a file
bool snapshotFile(string path, out FileSnapshot snapshot) {
	stat_t st;
	if (lstat(toStringz(path), &st) != 0 || !S_ISREG(st.st_mode)) {
		return false;
	}
	snapshot = FileSnapshot(st.st_ino, st.st_size, st.st_mtim.tv_sec, st.st_mtim.tv_nsec, st.st_nlink);
	return true;
}

private bool sameSnapshot(ref stat_t st, FileSnapshot snapshot) {
	return st.st_ino == snapshot.inode && st.st_size == snapshot.size
		&& st.st_mtim.tv_sec == snapshot.mtimeSec && st.st_mtim.tv_nsec == snapshot.mtimeNsec;
}

// Create a new placeholder, or refresh an existing placeholder in place, so that 'path'
// stands in for the given online item. Refuses to touch a path that holds real content:
// converting real content is convertToPlaceholder()'s job, which needs verified content.
//
// A new placeholder is fully built (marker, size, mode, time) under a filtered temporary
// name and then renamed into place without replacing anything, so no observer can ever see
// an unmarked file at 'path'. A refresh works on descriptors of the verified placeholder.
bool writePlaceholder(string path, long size, SysTime modifiedTime, PlaceholderIdentity identity, out string error) {
	version (linux) {
		string marker = encodeIdentity(identity);
		timespec[2] times = [toTimespec(modifiedTime), toTimespec(modifiedTime)];

		stat_t existing;
		if (lstat(toStringz(path), &existing) == 0) {
			if (!isOnDemandPlaceholder(path)) {
				error = "path holds local content; refusing to replace it with a placeholder";
				return false;
			}
			// Refresh in place. Open read-only first (the placeholder is read-only), verify the
			// marker on that descriptor, then open for writing and confirm it is the same inode.
			int fdRead = open(toStringz(path), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
			if (fdRead < 0) {
				error = errnoText("open");
				return false;
			}
			scope(exit) close(fdRead);
			if (fgetxattr(fdRead, toStringz(placeholderXattrName), null, 0) < 0) {
				error = "path stopped being a placeholder; not touching it";
				return false;
			}
			stat_t before;
			if (fstat(fdRead, &before) != 0 || fchmod(fdRead, octal!"600") != 0) {
				error = errnoText("fchmod");
				return false;
			}
			int fd = open(toStringz(path), O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
			if (fd < 0) {
				error = errnoText("open for refresh");
				fchmod(fdRead, placeholderFileMode);
				return false;
			}
			scope(exit) close(fd);
			stat_t after;
			if (fstat(fd, &after) != 0 || after.st_ino != before.st_ino) {
				error = "path was replaced during the refresh; not touching it";
				fchmod(fdRead, placeholderFileMode);
				return false;
			}
			if (fsetxattr(fd, toStringz(placeholderXattrName), marker.ptr, marker.length, 0) != 0
					|| ftruncate(fd, 0) != 0 || ftruncate(fd, size) != 0
					|| fchmod(fd, placeholderFileMode) != 0 || futimens(fd, times) != 0) {
				error = errnoText("placeholder refresh");
				fchmod(fd, placeholderFileMode);
				return false;
			}
			return true;
		}

		// New placeholder: build it under a temporary name that matches the default skip_file
		// rules ('.~*' and '*.partial'), then move it into place without replacing anything
		string temp = buildPath(dirName(path), ".~onedrive-placeholder-" ~ randomUUID().toString() ~ ".partial");
		int fd = open(toStringz(temp), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, octal!"600");
		if (fd < 0) {
			error = errnoText("create");
			return false;
		}
		bool built = fsetxattr(fd, toStringz(placeholderXattrName), marker.ptr, marker.length, 0) == 0
			&& ftruncate(fd, size) == 0 && fchmod(fd, placeholderFileMode) == 0 && futimens(fd, times) == 0;
		if (!built) {
			error = errnoText("building the placeholder") ~ "; does the filesystem support user extended attributes?";
		}
		close(fd);
		if (built && renameat2(AT_FDCWD, toStringz(temp), AT_FDCWD, toStringz(path), RENAME_NOREPLACE) != 0) {
			error = (errno == EEXIST) ? "a file appeared at this path; not replacing it" : errnoText("rename into place");
			built = false;
		}
		if (!built) {
			unlink(toStringz(temp));
		}
		return built;
	} else {
		error = "Files On-Demand is only supported on Linux";
		return false;
	}
}

// Convert a file that holds real content into a placeholder, in place (same inode, so
// --monitor sees a content change on an existing path, never a delete).
//
// 'verified' is a snapshot taken BEFORE the caller hashed the content and found it equal
// to the online file. The conversion only proceeds if the file is still exactly that file
// (same inode, size and modification time), has no other hard links, and no other program
// has it open: a write lease is taken for the duration, which fails if anyone else holds a
// descriptor. So no write can land between the content check and the truncation.
bool convertToPlaceholder(string path, FileSnapshot verified, SysTime modifiedTime, PlaceholderIdentity identity, out string error) {
	version (linux) {
		string marker = encodeIdentity(identity);
		timespec[2] times = [toTimespec(modifiedTime), toTimespec(modifiedTime)];
		int fd = open(toStringz(path), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0) {
			error = errnoText("open");
			return false;
		}
		scope(exit) close(fd);
		stat_t st;
		if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || !sameSnapshot(st, verified)) {
			error = "the file changed after it was checked; not freeing it";
			return false;
		}
		if (st.st_nlink != 1) {
			error = "the file has other hard links; not freeing it";
			return false;
		}
		if (fcntl(fd, F_SETLEASE, F_WRLCK) != 0) {
			error = (errno == EAGAIN) ? "the file is open in another program; not freeing it" : errnoText("taking a write lease");
			return false;
		}
		scope(exit) fcntl(fd, F_SETLEASE, F_UNLCK);
		if (fsetxattr(fd, toStringz(placeholderXattrName), marker.ptr, marker.length, 0) != 0) {
			error = errnoText("writing the placeholder attribute");
			return false;
		}
		fremovexattr(fd, toStringz(hydratedXattrName));
		if (ftruncate(fd, 0) != 0 || ftruncate(fd, verified.size) != 0
				|| fchmod(fd, placeholderFileMode) != 0 || futimens(fd, times) != 0) {
			error = errnoText("freeing the content");
			return false;
		}
		return true;
	} else {
		error = "Files On-Demand is only supported on Linux";
		return false;
	}
}

// Record that the file at 'path' holds the downloaded bytes of online item 'itemId'
bool markHydrated(string path, string itemId, string contentHash) {
	version (linux) {
		string value = itemId ~ "\n" ~ contentHash;
		return lsetxattr(toStringz(path), toStringz(hydratedXattrName), value.ptr, value.length, 0) == 0;
	} else {
		return false;
	}
}

// Forget the hydrated marker (the file is being uploaded, so it is local content now)
void clearHydratedMarker(string path) {
	version (linux) {
		lremovexattr(toStringz(path), toStringz(hydratedXattrName));
	}
}

// Does the file at 'path' still hold exactly the online bytes of item 'itemId' it was
// downloaded with? True means a difference from the local database baseline comes from the
// online side (the database has not caught up yet), not from a local edit.
bool isUnmodifiedHydratedFile(string path, string itemId) {
	version (linux) {
		string value;
		if (!readXattr(path, hydratedXattrName, value)) {
			return false;
		}
		auto parts = value.split("\n");
		if (parts.length != 2 || parts[0] != itemId || parts[1].length == 0) {
			return false;
		}
		string currentHash = computeQuickXorHash(path);
		if (currentHash.length > 0 && currentHash == parts[1]) {
			return true;
		}
		string currentSha256 = computeSHA256Hash(path);
		return currentSha256.length > 0 && currentSha256 == parts[1];
	} else {
		return false;
	}
}

// A fingerprint of the canonical content at 'path' for change detection around a
// download commit. A placeholder holds no content, so its fingerprint is its marker:
// hashing its zeros would be meaningless (and slow for large files).
string canonicalContentFingerprint(string path) {
	version (linux) {
		if (isOnDemandPlaceholder(path)) {
			string value;
			readXattr(path, placeholderXattrName, value);
			return "placeholder:" ~ value;
		}
	}
	return computeQuickXorHash(path);
}

// Serialise downloads of one placeholder across processes (the mount's helper, a running
// --monitor and --download-now). Returns a descriptor holding an exclusive lock on the
// current inode at 'path', or -1. Close it to release.
int lockOnDemandPath(string path) {
	version (linux) {
		int fd = open(toStringz(path), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0) {
			return -1;
		}
		if (flock(fd, LOCK_EX) != 0) {
			close(fd);
			return -1;
		}
		return fd;
	} else {
		return -1;
	}
}

void unlockOnDemandPath(int fd) {
	if (fd >= 0) {
		close(fd);
	}
}

// Map every placeholder below 'root' to its online item id. Used before deleting items
// online: a placeholder that was moved while no --monitor was watching makes its old path
// look deleted, and deleting it online would lose the only copy of its content.
string[string] collectPlaceholderItemIds(string root) {
	string[string] found;
	try {
		foreach (DirEntry entry; dirEntries(root, SpanMode.depth, false)) {
			try {
				if (!entry.isSymlink && entry.isFile && isOnDemandPlaceholder(entry.name)) {
					PlaceholderIdentity identity;
					if (readPlaceholderIdentity(entry.name, identity)) {
						found[identity.itemId] = entry.name;
					} else {
						// Unreadable marker: protect everything until it is repaired
						found["*"] = entry.name;
					}
				}
			} catch (FileException) {
				continue;
			}
		}
	} catch (FileException) {
	}
	return found;
}

// ---------------------------------------------------------------------------------
// --mount-on-demand: expose the sync directory through the FUSE bridge (src/fusebridge.c)
// ---------------------------------------------------------------------------------

// A download through the mount that makes no progress for 'on_demand_stall_timeout'
// seconds is stopped. Shorter limits would stop healthy downloads during a retry backoff.
immutable long minimumOnDemandStallTimeout = 10;

version (OnDemandFuse) {
	extern (C) int onedrive_fuse_mount(const(char)* backingRoot, const(char)* mountPoint, const(char)* hydrateExe, const(char)* confDir, int stallTimeout, int debugOutput);
}

int runOnDemandMount(ApplicationConfig appConfig, string syncDir, string confdirOption) {
	if (!appConfig.getValueBool("files_on_demand") || !onDemandSupportedPlatform()) {
		addLogEntry("ERROR: '--mount-on-demand' needs 'files_on_demand = \"true\"' in the config file (Linux only).");
		return EXIT_FAILURE;
	}
	string mountSetting = appConfig.getValueString("on_demand_mount_dir");
	if (mountSetting.empty) {
		addLogEntry("ERROR: '--mount-on-demand' needs 'on_demand_mount_dir' in the config file: the directory where the OneDrive files appear.");
		return EXIT_FAILURE;
	}
	string backing = buildNormalizedPath(absolutePath(expandTilde(syncDir)));
	string mountPoint = buildNormalizedPath(absolutePath(expandTilde(mountSetting)));
	if (mountPoint == backing || mountPoint.startsWith(backing ~ "/") || backing.startsWith(mountPoint ~ "/")) {
		addLogEntry("ERROR: 'on_demand_mount_dir' (" ~ mountPoint ~ ") and 'sync_dir' (" ~ backing ~ ") must not contain one another.");
		return EXIT_FAILURE;
	}
	if (!exists(backing) || !isDir(backing)) {
		addLogEntry("ERROR: The sync directory does not exist yet: " ~ backing ~ ". Run a sync first.");
		return EXIT_FAILURE;
	}
	if (!exists(mountPoint)) {
		mkdirRecurse(mountPoint);
	}
	string confDir = buildNormalizedPath(absolutePath(appConfig.configDirName));
	long stallTimeout = appConfig.getValueLong("on_demand_stall_timeout");
	if (stallTimeout < minimumOnDemandStallTimeout || stallTimeout > int.max) {
		addLogEntry("WARNING: 'on_demand_stall_timeout' must be at least " ~ to!string(minimumOnDemandStallTimeout) ~ " seconds; using " ~ to!string(minimumOnDemandStallTimeout));
		stallTimeout = minimumOnDemandStallTimeout;
	}

	version (OnDemandFuse) {
		addLogEntry("Files On-Demand mount: " ~ mountPoint ~ " -> " ~ backing ~ " (unmount with: fusermount3 -u " ~ mountPoint ~ ")");
		int rc = onedrive_fuse_mount(toStringz(backing), toStringz(mountPoint), toStringz(thisExePath()), toStringz(confDir), cast(int) stallTimeout, debugLogging ? 1 : 0);
		return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	} else {
		addLogEntry("ERROR: This build has no Files On-Demand mount support. Rebuild with: ./configure --enable-fuse (needs libfuse3).");
		return EXIT_FAILURE;
	}
}


// Placeholder lifecycle on a real filesystem (needs user extended attributes, e.g. ext4/btrfs/xfs/tmpfs)
version (linux) unittest {
	import std.datetime : SysTime, DateTime, UTC;
	import std.file : tempDir, mkdirRecurse, rmdirRecurse, write, read, getSize, timeLastModified, getAttributes, append;

	string dir = buildPath(tempDir(), "onedrive-ondemand-test-" ~ randomUUID().toString());
	mkdirRecurse(dir);
	scope(exit) rmdirRecurse(dir);
	SysTime t1 = SysTime(DateTime(2024, 1, 2, 3, 4, 5), UTC());
	SysTime t2 = SysTime(DateTime(2025, 6, 7, 8, 9, 10), UTC());
	string error;
	// Real id shapes: Business drive ids start with "b!", Personal item ids contain "!"
	auto businessId = PlaceholderIdentity("b!9Xa1-Zk_p2QxY", "01ABCDEF2GHIJ3KLMN", "aGFzaA==");
	auto personalId = PlaceholderIdentity("c64b3e8f9a1d2b7e", "C64B3E8F9A1D2B7E!101", "aGFzaDI=");

	// New placeholder: sparse, sized, timed, read-only, identified, and nothing left behind
	string ph = buildPath(dir, "a.bin");
	assert(writePlaceholder(ph, 1_000_000, t1, businessId, error), error);
	assert(isOnDemandPlaceholder(ph));
	assert(getSize(ph) == 1_000_000);
	assert(timeLastModified(ph) == t1);
	assert((getAttributes(ph) & octal!"777") == placeholderFileMode);
	stat_t st;
	assert(stat(toStringz(ph), &st) == 0 && st.st_blocks == 0, "placeholder must be sparse");
	PlaceholderIdentity identity;
	assert(readPlaceholderIdentity(ph, identity) && identity == businessId);
	assert(canonicalContentFingerprint(ph).startsWith("placeholder:"));
	size_t entries = 0;
	foreach (DirEntry e; dirEntries(dir, SpanMode.shallow)) {
		entries++;
	}
	assert(entries == 1, "no temporary file may be left behind");

	// A new placeholder never replaces a file that appeared at the path
	string taken = buildPath(dir, "taken.txt");
	write(taken, "x");
	assert(!writePlaceholder(taken, 5, t1, personalId, error));
	assert(cast(string) read(taken) == "x");

	// Refresh in place (online file changed): new size, time and hash, same inode
	ulong inode = st.st_ino;
	auto updated = businessId;
	updated.contentHash = "bmV3";
	assert(writePlaceholder(ph, 42, t2, updated, error), error);
	assert(stat(toStringz(ph), &st) == 0 && st.st_ino == inode && st.st_size == 42);
	assert(timeLastModified(ph) == t2);
	assert(readPlaceholderIdentity(ph, identity) && identity == updated);

	// An unparseable marker still counts as a placeholder (fail closed)
	string broken = buildPath(dir, "broken.bin");
	write(broken, "");
	string junk = "not-an-identity";
	assert(lsetxattr(toStringz(broken), toStringz(placeholderXattrName), junk.ptr, junk.length, 0) == 0);
	assert(isOnDemandPlaceholder(broken));
	assert(!readPlaceholderIdentity(broken, identity));
	assert(canonicalContentFingerprint(broken) == "placeholder:not-an-identity");

	// Real content is never overwritten by writePlaceholder
	string localFile = buildPath(dir, "local.txt");
	write(localFile, "local data");
	assert(!writePlaceholder(localFile, 5, t1, personalId, error));
	assert(cast(string) read(localFile) == "local data");
	assert(canonicalContentFingerprint(localFile) == computeQuickXorHash(localFile));

	// Free up space: refused once the file changed after the check, done in place otherwise
	FileSnapshot snap;
	assert(snapshotFile(localFile, snap));
	append(localFile, " edited after the check");
	assert(!convertToPlaceholder(localFile, snap, t1, personalId, error));
	assert(!isOnDemandPlaceholder(localFile));
	assert(snapshotFile(localFile, snap));
	assert(convertToPlaceholder(localFile, snap, t1, personalId, error), error);
	assert(isOnDemandPlaceholder(localFile));
	assert(stat(toStringz(localFile), &st) == 0 && st.st_ino == snap.inode && st.st_blocks == 0 && st.st_size == snap.size);
	assert(readPlaceholderIdentity(localFile, identity) && identity == personalId);

	// Free up space refuses hard-linked files
	string linked = buildPath(dir, "linked.txt");
	write(linked, "shared");
	assert(link(toStringz(linked), toStringz(buildPath(dir, "linked2.txt"))) == 0);
	assert(snapshotFile(linked, snap));
	assert(!convertToPlaceholder(linked, snap, t1, personalId, error));
	assert(cast(string) read(linked) == "shared");

	// Hydrated marker: bound to the item, unmodified until the content changes, clearable
	string hydrated = buildPath(dir, "h.txt");
	write(hydrated, "online bytes");
	assert(!isUnmodifiedHydratedFile(hydrated, personalId.itemId));
	assert(markHydrated(hydrated, personalId.itemId, computeQuickXorHash(hydrated)));
	assert(isUnmodifiedHydratedFile(hydrated, personalId.itemId));
	assert(!isUnmodifiedHydratedFile(hydrated, businessId.itemId), "marker must not apply to another item");
	append(hydrated, " + local edit");
	assert(!isUnmodifiedHydratedFile(hydrated, personalId.itemId));
	clearHydratedMarker(hydrated);

	// Index of placeholders by item id
	auto index = collectPlaceholderItemIds(dir);
	assert(index[businessId.itemId] == ph);
	assert(index[personalId.itemId] == localFile);
	assert("*" in index, "an unreadable marker must protect online deletes");

	// Locking a placeholder for download
	int lockFd = lockOnDemandPath(ph);
	assert(lockFd >= 0);
	unlockOnDemandPath(lockFd);

	// Directories and missing paths are never placeholders
	assert(!isOnDemandPlaceholder(dir));
	assert(!isOnDemandPlaceholder(buildPath(dir, "missing")));
}
