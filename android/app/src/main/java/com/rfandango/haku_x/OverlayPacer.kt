package com.rfandango.haku_x

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.View

/**
 * Keep overlay updates from delaying game presentation. With Turnip's raised KGSL priority,
 * an overlay frame on the lower-priority window queue can wait for a GPU gap and hold up game
 * frames behind it. PGR2 at 3x on Adreno 650 could stall presentation for over a second.
 *
 * After BUSY_MS without a gap, defer an overlay redraw for at most CAP_MS. If an overlay
 * frame is still waiting after STUCK_MS, call nativeGpuGap to let it run. Drivers with equal
 * queue priority report zero busy time and follow their normal pacing.
 */
class OverlayPacer(private val busyMs: () -> Int, private val gap: () -> Unit) {
  private val handler = Handler(Looper.getMainLooper())
  /** The last window frame drawn while the GPU was busy (uptime ms), 0: none to check. */
  private var drawnAt = 0L

  /** An overlay redraw wanted since `since` (uptime ms) may happen now. */
  fun mayDraw(since: Long): Boolean =
    busyMs() < BUSY_MS || SystemClock.uptimeMillis() - since >= CAP_MS

  /** Watches the frames of the window `view` is in. */
  fun attach(view: View) {
    view.viewTreeObserver.addOnDrawListener {
      if (busyMs() > 0) {
        drawnAt = SystemClock.uptimeMillis()
        handler.removeCallbacks(check)
        handler.postDelayed(check, STUCK_MS)
      }
    }
  }

  /*
   * The frame drawn at drawnAt: a break for it unless the GPU ran out of the
   * game's work after its GPU work was queued (SUBMIT_MS after the draw, as
   * the render thread draws it). A busy span begins where the GPU last ran out.
   */
  private val check = Runnable {
    val busy = busyMs()
    if (busy > 0 && SystemClock.uptimeMillis() - busy <= drawnAt + SUBMIT_MS) {
      gap()
    }
    drawnAt = 0L
  }

  companion object {
    /** The GPU busy this long without a break: the overlays wait to redraw. */
    const val BUSY_MS = 40
    /** An overlay redraw waits this long at most. */
    const val CAP_MS = 200L
    /** A window frame still waiting this long after its draw gets a break. */
    const val STUCK_MS = 32L
    /** How long after a draw its GPU work is queued, at most. */
    private const val SUBMIT_MS = 8L
  }
}
