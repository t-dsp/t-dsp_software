// Coalesce DOM writes into one requestAnimationFrame tick.
//
// The mixer surface receives meter blobs at 30 Hz (3 blobs per tick:
// /meters/input, /meters/output, /meters/host). Each blob fans out to
// many Signal subscribers, which previously wrote DOM properties
// synchronously inside `dispatcher.handleIncoming`. With 20 meter
// signals × 30 Hz that's 600 DOM writes/sec, all fighting for the
// main thread.
//
// `rafBatch(key, fn)` queues `fn` to run inside the next
// requestAnimationFrame callback, keyed by `key`. If the same key is
// queued multiple times before the frame fires, only the LAST `fn`
// runs — older pending writes are discarded. This naturally collapses
// rapid signal updates (e.g. five `peakL` updates inside one task)
// into a single DOM write.
//
// Pick `key` to be a stable identity per logical "thing being updated"
// — typically the DOM element you're writing to. Two updates to the
// same fill div get one write per frame; updates to different
// elements run independently.
//
// Cap: at most 60 writes/sec per key on a 60 Hz display, regardless
// of how fast Signals are firing.

type Updater = () => void;

const pending = new Map<unknown, Updater>();
let scheduled = false;

function flush(): void {
  scheduled = false;
  // Snapshot before iterating in case an updater enqueues another
  // pending write (it would land in the next frame, not this one).
  const snapshot = Array.from(pending.values());
  pending.clear();
  for (const fn of snapshot) {
    try {
      fn();
    } catch (e) {
      // Don't let one broken subscriber kill the rest of the frame.
      // eslint-disable-next-line no-console
      console.error('rafBatch updater threw:', e);
    }
  }
}

export function rafBatch(key: unknown, fn: Updater): void {
  pending.set(key, fn);
  if (!scheduled) {
    scheduled = true;
    requestAnimationFrame(flush);
  }
}

// For tests / teardown only.
export function _rafBatchClear(): void {
  pending.clear();
  scheduled = false;
}
