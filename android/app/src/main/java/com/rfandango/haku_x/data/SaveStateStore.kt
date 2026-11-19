package com.rfandango.haku_x.data

import android.content.Context
import android.util.Log
import java.io.File
import java.security.MessageDigest

/**
 * Store ten save-state slots per title ID and version. Each slot holds the core's state file
 * and the frame captured at save time. Using GameOverrides.gameKey lets the library and game
 * find the same slots. Migrate old slots keyed by the hash of discUri on first access.
 */
class SaveStateStore(context: Context, gameKey: String, discUri: String? = null) {
  private val root = File(context.filesDir, "native/states")
  private val dir = File(root, folder(gameKey))

  init {
    if (discUri != null) adoptLegacy(File(root, hash(discUri)))
  }

  fun state(slot: Int) = File(dir, "slot$slot.hxs")
  fun thumb(slot: Int) = File(dir, "slot$slot.jpg")

  /** The slots holding a state. */
  fun saved(): List<Int> = (1..SLOTS).filter { state(it).isFile }

  /** Where a slot's files go (made on first use). */
  fun prepare() {
    dir.mkdirs()
  }

  /**
   * A slot's state and frame go. The game's cache copies (blobs/: what its
   * states' cache drives held, one copy per content) go with its last state;
   * with states left, the next save drops the copies none of them uses.
   */
  fun delete(slot: Int) {
    state(slot).delete()
    thumb(slot).delete()
    if (saved().isEmpty()) File(dir, "blobs").deleteRecursively()
  }

  /** States kept by the disc URI's hash: their slots move here (newer wins). */
  private fun adoptLegacy(old: File) {
    if (old == dir || !old.isDirectory) return
    dir.mkdirs()
    for (slot in 1..SLOTS) {
      val s = File(old, "slot$slot.hxs")
      if (!s.isFile) continue
      if (!state(slot).isFile || s.lastModified() > state(slot).lastModified()) {
        s.renameTo(state(slot))
        File(old, "slot$slot.jpg").let { if (it.isFile) it.renameTo(thumb(slot)) }
        Log.i(TAG, "savestate slot $slot moved from ${old.name} to ${dir.name}")
      }
    }
    old.listFiles()?.forEach { it.delete() }
    old.delete()
  }

  companion object {
    const val SLOTS = 10
    private const val TAG = "hakuX-library"

    /** The slot a state file is (state's slot<N>.hxs), or null. */
    fun slotOf(path: String): Int? = Regex("""slot(\d+)\.hxs$""").find(path)?.groupValues?.get(1)?.toIntOrNull()

    /** A folder name for a key: as it is when it can be ("4D53004B.1"), else hashed. */
    private fun folder(key: String): String =
      if (key.matches(Regex("[A-Za-z0-9._-]{1,64}"))) key else hash(key)

    private fun hash(s: String): String {
      val d = MessageDigest.getInstance("SHA-1").digest(s.toByteArray())
      return d.take(8).joinToString("") { "%02x".format(it) }
    }
  }
}
