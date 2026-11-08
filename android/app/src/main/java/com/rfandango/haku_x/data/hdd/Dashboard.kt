package com.rfandango.haku_x.data.hdd

import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.ZipInputStream

/**
 * Dashboard files live on C: (Partition2). The core supplies a dashboard for an empty disk.
 * Insignia account setup still requires the user's Xbox dashboard files.
 */
object Dashboard {
  /** NativeCoreActivity's disc for the dashboard: none, the core booting C:\xboxdash.xbe. */
  const val URI = "hakux:dashboard"
  const val XBE = "xboxdash.xbe"

  fun cDrive(root: File) = XboxHdd.folder(root, 2)

  /** The certificate title of hakuX-lr's own dashboard (native/core/dash.c). */
  private const val OURS = "hakux-dash"

  /** Check for a user-supplied dashboard for Insignia account setup. */
  fun installed(root: File) =
    cDrive(root).listFiles()?.any { it.isFile && it.name.equals(XBE, ignoreCase = true) && !ours(it) } == true

  private fun ours(xbe: File): Boolean = runCatching {
    RandomAccessFile(xbe, "r").use { f ->
      val head = ByteArray(0x120)
      f.readFully(head)
      val b = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN)
      if (b.getInt(0) != 0x48454258) return false   // "XBEH"
      val cert = b.getInt(0x118).toLong() - b.getInt(0x104).toLong()
      val name = ByteArray(2 * (OURS.length + 1))
      f.seek(cert + 0xC)
      f.readFully(name)
      String(name, Charsets.UTF_16LE) == OURS + "\u0000"
    }
  }.getOrDefault(false)

  /**
   * The dashboard's files from a zip (open() reads it from the start, twice) into C:, over what's
   * there: the files beside its xboxdash.xbe and the folders under it, from the zip's root or a
   * folder inside. Names are matched in any case, as the core matches them (an XODash already there
   * takes xodash's files).
   */
  fun install(open: () -> InputStream, root: File, progress: XboxHdd.Progress): XboxHdd.Result {
    // where xboxdash.xbe is, and how much there is under it
    var prefix: String? = null
    val entries = ArrayList<Pair<String, Long>>()
    ZipInputStream(open()).use { zip ->
      generateSequence { zip.nextEntry }.forEach { e ->
        val name = e.name.replace('\\', '/')
        entries += name to e.size.coerceAtLeast(0)
        if (!e.isDirectory && name.substringAfterLast('/').equals(XBE, ignoreCase = true)) {
          val p = name.substring(0, name.length - XBE.length)
          if (prefix == null || p.length < prefix!!.length) prefix = p
        }
      }
    }
    val base = prefix ?: throw IOException("no $XBE in this zip: not the dashboard's files")
    val total = entries.filter { it.first.startsWith(base, ignoreCase = true) }.sumOf { it.second }.coerceAtLeast(1)
    val c = cDrive(root).apply { mkdirs() }
    var files = 0
    var bytes = 0L
    val skipped = ArrayList<String>()
    val buf = ByteArray(1 shl 16)
    ZipInputStream(open()).use { zip ->
      generateSequence { zip.nextEntry }.forEach { e ->
        val name = e.name.replace('\\', '/')
        if (!name.startsWith(base, ignoreCase = true)) return@forEach
        val parts = name.substring(base.length).split('/').filter { it.isNotEmpty() }
        if (parts.isEmpty()) return@forEach
        if (parts.any { it == "." || it == ".." }) {
          skipped += name
          return@forEach
        }
        if (e.isDirectory) {
          resolve(c, parts, dir = true)
          return@forEach
        }
        val out = resolve(c, parts, dir = false)
        progress.update(bytes, total, parts.joinToString("\\"))
        out.outputStream().use { o ->
          while (true) {
            val n = zip.read(buf)
            if (n < 0) break
            o.write(buf, 0, n)
            bytes += n
          }
        }
        if (e.time > 0) out.setLastModified(e.time)
        files++
      }
    }
    progress.update(total, total, "")
    return XboxHdd.Result(files, bytes, skipped)
  }

  /** The path parts under top, each an existing name in any case or made: the folders made, a file (dir false) not. */
  private fun resolve(top: File, parts: List<String>, dir: Boolean): File {
    var at = top
    parts.forEachIndexed { i, p ->
      val last = i == parts.size - 1
      val found = at.listFiles()?.firstOrNull { it.name.equals(p, ignoreCase = true) }
      at = found ?: File(at, p)
      if ((!last || dir) && !at.isDirectory) {
        if (at.exists()) at.delete()
        at.mkdirs()
      }
    }
    return at
  }
}
