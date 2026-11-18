# Files On-Demand

> [!NOTE]
> Files On-Demand is a feature of the BigGreenDelta fork of the OneDrive Client for Linux. It is not part of the upstream client.

Files On-Demand works like the Windows OneDrive feature of the same name. Your whole OneDrive is visible locally, but a file's content is only downloaded when you open it, or when you choose to keep it on this device. Files you don't use take no disk space.

| Windows OneDrive | This client |
|---|---|
| Settings > Files On-Demand | `files_on_demand = "true"` in the config file |
| Online-only file (cloud icon) | **Placeholder**: correct name, size and date, but no content on disk |
| Open an online-only file | Open it through the on-demand mount (`--mount-on-demand`): it downloads first, then opens |
| Locally available file | A file that holds its content, after you opened or downloaded it |
| "Always keep on this device" | A matching rule in the `pin_list` file |
| "Free up space" | `onedrive --free-up-space <path>` |
| Download a file without opening it | `onedrive --download-now <path>` |

## How it works

There are three parts:

1. **The sync directory (`sync_dir`)** holds real files and placeholders. The normal client (`--sync` or `--monitor`) keeps it in step with OneDrive. A placeholder is created instead of downloading a file when the file is not pinned and the path holds no local content yet.
2. **The on-demand mount (`on_demand_mount_dir`)** shows the sync directory with one extra behaviour: when an application opens a placeholder, the mount downloads it first, so the application reads the real content. You use your files through this directory.
3. **`pin_list`** names the folders and files that must always be on this device. They are downloaded in full, like a normal sync.

The mount is local-disk passthrough. It never contacts OneDrive except to download a placeholder that is being opened. Listing folders, and reading files that are already on this device, runs at local disk speed and keeps working offline.

A placeholder is a sparse file: it reports the online file size but uses no disk blocks. It is read-only and carries the extended attribute `user.onedrive.placeholder`, which records the online item it stands for (drive id, item id and content hash, one per line). Any file carrying that attribute is treated as having no content, even if the attribute cannot be read.

## Setup

### 1. Build with mount support

```text
./configure --enable-fuse
make
```

This needs `libfuse3` with its headers (Arch: `fuse3`; Debian/Ubuntu: `libfuse3-dev`). Without `--enable-fuse` everything works except `--mount-on-demand`.

### 2. Configure

Use a hidden directory as the sync directory, and mount it where you want to see your OneDrive:

```text
# ~/.config/onedrive/config
sync_dir = "~/.local/share/onedrive/OneDrive"
files_on_demand = "true"
on_demand_mount_dir = "~/OneDrive"
```

`sync_dir` must be on a filesystem with user extended attributes (ext4, btrfs, xfs and tmpfs all have them). `sync_dir` and `on_demand_mount_dir` must not contain one another.

### 3. Pin what must always be on this device

`~/.config/onedrive/pin_list` uses the same rule syntax as `sync_list` ([sync_list documentation](usage.md#performing-a-selective-synchronisation-via-sync_list-file)). For example:

```text
# Always keep on this device
/Documents/Notes/
/.personal/library/
```

Pinned files are always downloaded in full. A pinned placeholder is downloaded at the next full scan (every `--sync`, and every `monitor_fullscan_frequency` cycles under `--monitor`).

### 4. Run it

Sign in once (`onedrive`), then run the sync and the mount, for example as systemd user services:

```text
onedrive --monitor
onedrive --mount-on-demand
```

A user unit for the mount:

```ini
# ~/.config/systemd/user/onedrive-ondemand.service
[Unit]
Description=OneDrive Files On-Demand mount
After=network-online.target

[Service]
ExecStart=/usr/local/bin/onedrive --mount-on-demand
ExecStop=/usr/bin/fusermount3 -u %h/OneDrive
Restart=on-failure

[Install]
WantedBy=default.target
```

## Commands

| Command | What it does |
|---|---|
| `onedrive --download-now <path>` | Downloads a placeholder. On a folder, downloads every placeholder in it. |
| `onedrive --free-up-space <path>` | Turns an unchanged local file back into a placeholder. On a folder, does it for every file in it. Pinned files and files with unsynced edits are skipped. |
| `onedrive --mount-on-demand` | Mounts `sync_dir` at `on_demand_mount_dir`. Runs in the foreground until unmounted (`fusermount3 -u <dir>`). |

`<path>` can be inside `on_demand_mount_dir` or inside `sync_dir`. These commands can run while `--monitor` is running. They never start an interactive sign-in: sign in first.

## Browsing does not download

Opening an online-only file downloads it, but a file manager opens files only to look at them: to draw thumbnails and to tell a file's type. The mount refuses those reads so browsing a folder never downloads it, like Windows:

- thumbnailers (`glycin…`, any program with `thumb` in its name) and desktop indexers (`localsearch`, `tracker…`) get "Input/output error" for an online-only file, and show a generic icon;
- the file manager itself (`nautilus`) is refused too: it reads files only to tell their type, one after another, and each download takes seconds;
- every other program, and any file you open from the file manager, downloads as usual.

## Free up space automatically

Like Windows Storage Sense, `--monitor` makes files online-only again after every cycle:

- `on_demand_free_after_days` (default 0, off): a file not opened for that many days is freed.
- `on_demand_cache_max_gb` (default 10, 0 = no limit): when files opened on demand use more than this, the least recently opened are freed until the rest fit.

Only files downloaded for Files On-Demand are freed: opened through the mount, or fetched with `--download-now`. Pinned files, files you created or edited here, files opened in the last 10 minutes and files whose bytes differ from the online file are never freed. Each file is checked against the online file first, exactly as `--free-up-space` does, and nothing is freed while OneDrive is unreachable.

The mount records an open as the file's access time (the sync folder is often mounted `noatime`, so the kernel would not), at most once an hour per file. The sync ignores access times.

## What happens to existing files

Turning `files_on_demand` on never removes content: files that are already on this device stay there. Only new online files, and files whose path holds nothing yet, become placeholders. To free space, run `--free-up-space` on the folders you don't need locally.

## Safety rules

- A placeholder is never uploaded, never hashed as content, and never backed up as "local data to preserve". Deleting a placeholder through the mount deletes the file online, like deleting any synced file.
- An online delete is refused while a placeholder for that item (or for anything inside that folder) still exists somewhere in the sync directory. That is what a placeholder moved while `--monitor` was not running looks like, and deleting it online would lose the only copy of its content. The client logs a warning naming the placeholder: move it back, or run `--download-now` on it first. Run `--monitor` while you work through the mount and moves are handled normally.
- `--free-up-space` checks that the local bytes equal the current online file before removing them. It refuses files with local edits, files that change after the check, files with other hard links, and files another program has open.
- A download replaces the placeholder in one atomic rename, and only if it is still the same placeholder of the same version. If the file changed locally, or the placeholder was refreshed to a newer version, while it was downloading, the download is discarded. Downloads of the same placeholder by different processes are serialised with a file lock.
- A file downloaded on demand records its item id and the hash of the downloaded bytes (`user.onedrive.hydrated`). Until the sync catches up with the online version it came from, the client recognises it as unchanged online content and does not upload it. Once you edit it, it uploads normally, and the marker is removed.
- Downloads on open respect `space_reservation`, `disable_download_validation` and the malware flag, like normal downloads.

## Status for desktop tools

Two JSON files in the config directory report what is moving, for a status bar or a script. Each is replaced atomically, so a reader never sees half a file, and each carries the `pid` of its writer: a file whose process is gone is stale.

- `monitor-status.json`, from `--monitor` (see [monitor_status](application-config-options.md#monitor_status)). This fork adds a `transfers` block:
  - `uploads_pending`, `downloads_pending`: files queued or in transfer in the current pass, updated after every batch of `threads` files.
  - `local_changes_pending`: `true` from the moment the local watcher reports a change until the monitor has applied it. A saved file waits in inotify batching (up to 5 seconds, plus `inotify_delay` when `delay_inotify_processing` is on) before it becomes a counted upload; this flag covers that gap.
  - `upload_failures_unresolved`, `download_failures_unresolved`: failed transfers not yet retried.
  - and an `on_demand` block, refreshed after every cycle by the automatic free-up pass: `pinned_bytes` (files covered by `pin_list`), `opened_bytes` (files downloaded because they were opened, the ones the pass may free), `other_bytes` (other local content), `cache_max_gb` and `free_after_days` (the settings in use), `last_cleanup_at`, `last_cleanup_freed_files` and `last_cleanup_freed_bytes`. `usage_known` is `false` until the first pass has run.
- `ondemand-status.json`, from `--mount-on-demand`: `downloads_pending` counts files waiting for or in a download because they were opened. Several opens of one file count once. Removed when the mount stops.

Everything is uploaded only when `process.state` is `idle`, all counts are 0 and `local_changes_pending` is `false`.


- Use your files through `on_demand_mount_dir`. Reading a placeholder directly in `sync_dir` returns zeros, because a placeholder has no content.
- Opening a placeholder without network access fails. When a download receives no new data for `on_demand_stall_timeout` seconds (default 60), it is stopped and the open fails with "Connection timed out"; other download failures give "Input/output error". The limit is on progress, not on total time, so a large file on a slow connection is never stopped while data keeps arriving. Opening a stopped large file (4 MiB or more) again resumes where the download stopped.
- For 30 seconds after a failed download, opening the same file fails at once ("Input/output error") instead of retrying. Files that are already on this device keep working offline.
- Opening a placeholder for writing (including `touch`) downloads it first. Changing a placeholder's mode or time without opening it is ignored, and hard links to placeholders are refused.
- At most 4 placeholders download at once through the mount; further opens wait for a free slot.
- `--download-now`, `--free-up-space` and `--mount-on-demand` do not support `--dry-run`.
- Linux only.
- There are no file-manager overlay icons. A placeholder shows its full size, but `du` counts it as 0 bytes.
