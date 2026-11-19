package com.rfandango.haku_x.data.hdd

import java.io.File
import java.io.FilterInputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.Properties
import java.util.zip.Deflater
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/**
 * Export the native console disk as a ZIP, with partition folders, raw partition heads and
 * eeprom.bin under hdd/. Optionally include states/ and omit cache drives, which games
 * rebuild. Keep the existing save signatures so backup and restore need no BIOS. The first
 * ZIP entry is [MARKER], as in SaveArchive's ZIPs.
 */
object Backup {
  const val MARKER = "hakuX.properties"

  /** A hakuX zip's marker: "backup" (this) or "saves" (SaveArchive), when made, and what it holds. */
  class Info(val kind: String, val created: Long, val states: Boolean, val caches: Boolean)

  private fun isCache(name: String) = name.matches(Regex("(?i)partition[345]|raw-partition[345]\\.bin"))

  /** The marker entry, first in a zip. */
  fun putMarker(zip: ZipOutputStream, info: Info) {
    val p = Properties()
    p["format"] = "1"
    p["kind"] = info.kind
    p["created"] = info.created.toString()
    p["states"] = info.states.toString()
    p["caches"] = info.caches.toString()
    zip.putNextEntry(ZipEntry(MARKER).apply { time = info.created })
    p.store(zip, "hakuX")
    zip.closeEntry()
  }

  /** A marker's contents, from its entry's stream. */
  fun parse(z: InputStream): Info {
    val p = Properties().apply { load(z) }
    return Info(p.getProperty("kind", ""), p.getProperty("created", "0").toLongOrNull() ?: 0,
                p.getProperty("states") == "true", p.getProperty("caches") == "true")
  }

  /** A zip's marker, or null when it isn't a hakuX zip. */
  fun info(input: InputStream): Info? = ZipInputStream(input.buffered()).use { z ->
    val e = z.nextEntry ?: return null
    if (e.name == MARKER) parse(z) else null
  }

  /** What a backup of native holds: (its name in the zip, the file or folder). */
  fun entries(native: File, states: Boolean, caches: Boolean): List<Pair<String, File>> {
    val out = ArrayList<Pair<String, File>>()
    fun walk(name: String, f: File) {
      out += name to f
      if (f.isDirectory) for (c in f.listFiles().orEmpty().sortedBy { it.name }) walk("$name/${c.name}", c)
    }
    val hdd = File(native, "hdd")
    out += "hdd" to hdd
    for (c in hdd.listFiles().orEmpty().sortedBy { it.name }) {
      if (caches || !isCache(c.name)) walk("hdd/${c.name}", c)
    }
    if (states) File(native, "states").takeIf { it.isDirectory }?.let { walk("states", it) }
    return out
  }

  /** The backup of native written to out. */
  fun write(out: OutputStream, native: File, states: Boolean, caches: Boolean, progress: XboxHdd.Progress): XboxHdd.Result {
    val list = entries(native, states, caches)
    val total = list.sumOf { (_, f) -> if (f.isFile) f.length() else 0L }
    var done = 0L
    var files = 0
    val buf = ByteArray(1 shl 16)
    ZipOutputStream(out.buffered(1 shl 16)).use { zip ->
      zip.setLevel(Deflater.BEST_SPEED)
      putMarker(zip, Info("backup", System.currentTimeMillis(), states, caches))
      for ((name, f) in list) {
        zip.putNextEntry(ZipEntry(if (f.isDirectory) "$name/" else name).apply { time = f.lastModified() })
        if (f.isFile) {
          f.inputStream().use { i ->
            while (true) {
              val n = i.read(buf)
              if (n < 0) break
              zip.write(buf, 0, n)
              done += n
              progress.update(done, total, name)
            }
          }
          files++
        }
        zip.closeEntry()
      }
    }
    return XboxHdd.Result(files, done, emptyList())
  }

  private class Counting(i: InputStream) : FilterInputStream(i) {
    var count = 0L
    override fun read(): Int = super.read().also { if (it >= 0) count++ }
    override fun read(b: ByteArray, off: Int, len: Int): Int = super.read(b, off, len).also { if (it > 0) count += it }
    override fun skip(n: Long): Long = super.skip(n).also { count += it }
  }

  /**
   * The backup in input (size bytes, for the progress) restored over native: written aside first, then
   * swapped in for the disk and, if it has them, the savestates. Cache drives it doesn't have stay.
   */
  fun restore(input: InputStream, size: Long, native: File, progress: XboxHdd.Progress): XboxHdd.Result {
    val tmp = File(native, ".restore")
    tmp.deleteRecursively()
    tmp.mkdirs()
    val root = tmp.canonicalPath + File.separator
    var info: Info? = null
    val dirTimes = ArrayList<Pair<File, Long>>()
    val skipped = ArrayList<String>()
    var files = 0
    var bytes = 0L
    val buf = ByteArray(1 shl 16)
    try {
      val counted = Counting(input)
      ZipInputStream(counted.buffered(1 shl 16)).use { z ->
        while (true) {
          val e = z.nextEntry ?: break
          if (info == null) {
            if (e.name != MARKER) throw IOException("not a hakuX backup")
            info = parse(z).also { if (it.kind != "backup") throw IOException("not a hakuX backup") }
            continue
          }
          val name = e.name.replace('\\', '/').trimEnd('/')
          val f = File(tmp, name)
          if (!(name == "hdd" || name.startsWith("hdd/") || name == "states" || name.startsWith("states/")) ||
              !(f.canonicalPath + File.separator).startsWith(root)) {
            skipped += e.name
            continue
          }
          if (e.isDirectory) {
            f.mkdirs()
            if (e.time != -1L) dirTimes += f to e.time
          } else {
            f.parentFile!!.mkdirs()
            f.outputStream().use { o ->
              while (true) {
                val n = z.read(buf)
                if (n < 0) break
                o.write(buf, 0, n)
                bytes += n
                progress.update(counted.count, size, name)
              }
            }
            if (e.time != -1L) f.setLastModified(e.time)
            files++
          }
        }
      }
      val i = info ?: throw IOException("not a hakuX backup")
      for ((d, t) in dirTimes.asReversed()) d.setLastModified(t)

      swap(File(native, "hdd"), File(tmp, "hdd").apply { mkdirs() }) { old, cur ->
        if (!i.caches) for (c in old.listFiles().orEmpty()) if (isCache(c.name)) c.renameTo(File(cur, c.name))
      }
      if (i.states) swap(File(native, "states"), File(tmp, "states").apply { mkdirs() })
    } finally {
      tmp.deleteRecursively()
    }
    return XboxHdd.Result(files, bytes, skipped)
  }

  /** cur replaced by new: the old one is set aside until new is in place, and keep takes what stays of it. */
  private fun swap(cur: File, new: File, keep: (old: File, cur: File) -> Unit = { _, _ -> }) {
    val old = File(cur.parentFile, ".restore-old-${cur.name}")
    old.deleteRecursively()
    if (cur.exists() && !cur.renameTo(old)) throw IOException("can't move ${cur.name} aside")
    if (!new.renameTo(cur)) {
      old.renameTo(cur)
      throw IOException("can't put the restored ${cur.name} in place")
    }
    if (old.exists()) keep(old, cur)
    old.deleteRecursively()
  }
}
