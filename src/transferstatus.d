// What do we know about this module?
// Fork feature: counts of file transfers this process still has to finish, published in
// monitor-status.json ("transfers") so a status bar can show pending uploads and downloads.
module transferstatus;

// The counters are updated only by the thread that runs the transfer batch loops (the
// main thread), between batches, so they need no locking. A batch is 'threads' files.
long uploadsPending = 0;
long downloadsPending = 0;
// True from the moment the local watcher reports a change until the monitor has applied it.
// Changes wait in inotify batching (and 'inotify_delay') before they become counted uploads;
// this keeps a just-saved file from ever reading as "everything uploaded".
bool localChangesPending = false;

// Set by main.d while --monitor publishes its status; called after every change.
void delegate() transferStatusChanged = null;

enum TransferKind { upload, download }

private ref long counterFor(TransferKind kind) {
	return kind == TransferKind.upload ? uploadsPending : downloadsPending;
}

private void publish() {
	if (transferStatusChanged !is null) {
		transferStatusChanged();
	}
}

// 'count' files are about to be transferred
void transfersQueued(TransferKind kind, size_t count) {
	if (count == 0) return;
	counterFor(kind) += count;
	publish();
}

// 'count' queued files are finished: transferred, failed or skipped
void transfersFinished(TransferKind kind, size_t count) {
	if (count == 0) return;
	long* counter = &counterFor(kind);
	*counter = (*counter > count) ? *counter - count : 0;
	publish();
}

void setLocalChangesPending(bool pending) {
	if (localChangesPending == pending) return;
	localChangesPending = pending;
	publish();
}
