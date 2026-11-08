package com.rfandango.haku_x.data.hdd

import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/**
 * Game saves as a zip laid out like the Xbox's E:\UDATA (UDATA/<title id>/<save>/...), the form save
 * collections and FTP copies to a console use. A title's own files (TitleMeta.xbx, TitleImage.xbx, its
 * default SaveImage.xbx) go with any of its saves, so the dashboard can name them.
 *
 * Imports take the same zips and the usual variants: without the UDATA folder, inside another folder,
 * with E:\TDATA's title folders beside. A save in the zip replaces the save of the same folder on the disk.
 * hakuX ZIPs start with [Backup.MARKER]. Full backup imports extract E: and read the source EEPROM key.
 */
object SaveArchive {
  /** A title's folder (E:\UDATA\<title id>) and which of its save folders to write: null, all of them. */
  class Pick(val title: File, val saves: List<File>? = null)

  /** Stage copies of saves that need re-signing with [SaveResigner.LEGACY_HD] for export. */
  fun portable(picks: List<Pick>, hd: ByteArray, stage: File): List<Pick> {
    if (hd.contentEquals(SaveResigner.LEGACY_HD)) return picks
    val e = File(stage, "E")
    val out = picks.map { p ->
      val title = File(e, "UDATA/${p.title.name}").apply { mkdirs() }
      val saves = ArrayList<File>()
      for (f in p.title.listFiles().orEmpty()) {
        when {
          f.isFile -> f.copyTo(File(title, f.name), overwrite = true).setLastModified(f.lastModified())
          p.saves == null || f in p.saves -> {
            val to = File(title, f.name)
            f.copyRecursively(to, overwrite = true)
            f.walkTopDown().forEach { src -> File(to, src.relativeTo(f).path).setLastModified(src.lastModified()) }
            saves += to
          }
        }
      }
      title.setLastModified(p.title.lastModified())
      Pick(title, if (p.saves == null) null else saves)
    }
    SaveResigner.hakux(hd, SaveResigner.LEGACY_HD).resign(e, e.walkTopDown().filter { it.isFile }.toList()) { null }
    return out
  }

  /** The picks written to out as a zip; the number of files in it. */
  fun write(out: OutputStream, picks: List<Pick>): Int {
    var files = 0
    ZipOutputStream(out.buffered()).use { zip ->
      Backup.putMarker(zip, Backup.Info("saves", System.currentTimeMillis(), states = false, caches = false))

      fun add(path: String, f: File) {
        zip.putNextEntry(ZipEntry(if (f.isDirectory) "$path/" else path).apply { time = f.lastModified() })
        if (f.isFile) {
          f.inputStream().use { it.copyTo(zip) }
          files++
        }
        zip.closeEntry()
      }

      fun tree(path: String, dir: File) {
        add(path, dir)
        for (f in dir.listFiles().orEmpty().sortedBy { it.name }) {
          if (f.isDirectory) tree("$path/${f.name}", f) else add("$path/${f.name}", f)
        }
      }

      for (p in picks) {
        val base = "UDATA/${p.title.name}"
        add(base, p.title)
        for (f in p.title.listFiles().orEmpty().sortedBy { it.name }) {
          when {
            f.isFile -> add("$base/${f.name}", f)
            p.saves == null || f in p.saves -> tree("$base/${f.name}", f)
          }
        }
      }
    }
    return files
  }

  /**
   * What a zip of saves holds: its saves (title id to save folders), every title it has files of, the
   * names its TitleMeta.xbx give, how many of its saves replace one on the disk, its files, entries left out,
   * and its marker's kind: "saves" or "backup" for hakuX's zips, null for others (a console's or xemu's).
   * Full backups include the source EEPROM so imports can re-sign saves.
   */
  class Plan(
    val kind: String?,
    val saves: Map<String, Set<String>>,
    val titles: Set<String>,
    val names: Map<String, String>,
    val replaced: Int,
    val files: Int,
    val skipped: List<String>,
    val eeprom: ByteArray? = null,
  ) {
    val saveCount get() = saves.values.sumOf { it.size }
  }

  /** An import's outcome: the files it wrote, and the zip's entries left out. */
  class Result(val written: List<File>, val skipped: List<String>) {
    val files get() = written.size
  }

  /** A zip entry's place on E: (UDATA or TDATA, title id, the path below the title), or null. */
  private class Place(val area: String, val title: String, val rest: List<String>, val dir: Boolean) {
    /** The save folder the entry belongs to: UDATA/<title>/<save>/... */
    val save get() = if (area == "UDATA" && rest.size >= (if (dir) 1 else 2)) rest[0] else null
  }

  private val TITLE = Regex("[0-9A-Fa-f]{8}")

  /** Under the first folder named like a title id (8 hex digits); TDATA if the folder above it says so. */
  private fun place(e: ZipEntry): Place? {
    val parts = e.name.replace('\\', '/').split('/').filter { it.isNotEmpty() }
    val i = parts.indexOfFirst { TITLE.matches(it) }
    if (i < 0) return null
    val rest = parts.drop(i + 1)
    if (rest.any { it == "." || it == ".." || it.length > 42 }) return null   // FATX names: 42 characters
    if (rest.isEmpty() && !e.isDirectory) return null
    val area = if (i > 0 && parts[i - 1].equals("TDATA", ignoreCase = true)) "TDATA" else "UDATA"
    return Place(area, parts[i].lowercase(), rest, e.isDirectory)
  }

  /**
   * Visit ZIP entries with their E: paths, or null outside title folders. Return the archive kind.
   * For full backups, visit only E: entries and pass the EEPROM to [eeprom] separately.
   */
  private inline fun entries(input: InputStream, eeprom: (ByteArray) -> Unit,
                             each: (ZipInputStream, ZipEntry, Place?) -> Unit): String? {
    var kind: String? = null
    ZipInputStream(input.buffered()).use { z ->
      var first = true
      while (true) {
        val en = z.nextEntry ?: break
        if (first && en.name == Backup.MARKER) {
          kind = Backup.parse(z).kind
          first = false
          continue
        }
        first = false
        if (kind == "backup" && !en.name.startsWith("hdd/Partition1/", ignoreCase = true)) {
          if (en.name == "hdd/eeprom.bin") eeprom(z.readBytes())
          continue
        }
        each(z, en, place(en))
      }
    }
    return kind
  }

  /** A folder's child in any case, as the core matches names (io.c host_path), or a new one. */
  private fun child(dir: File, name: String) =
    dir.listFiles()?.firstOrNull { it.name.equals(name, ignoreCase = true) } ?: File(dir, name)

  /** What importing the zip in input into E: (partition 1's folder) would do. */
  fun plan(input: InputStream, e: File): Plan {
    val saves = LinkedHashMap<String, MutableSet<String>>()
    val titles = LinkedHashSet<String>()
    val names = HashMap<String, String>()
    val skipped = ArrayList<String>()
    var files = 0
    var eeprom: ByteArray? = null
    val kind = entries(input, { eeprom = it }) { z, en, p ->
      if (p == null) {
        if (!en.isDirectory) skipped += en.name
        return@entries
      }
      if (!en.isDirectory) files++
      titles += p.title
      p.save?.let { saves.getOrPut(p.title) { LinkedHashSet() } += it.lowercase() }
      if (p.area == "UDATA" && p.rest.size == 1 && p.rest[0].equals("TitleMeta.xbx", ignoreCase = true)) {
        HddStore.meta(z.readBytes(), "TitleName")?.let { names[p.title] = it }
      }
    }
    val udata = child(e, "UDATA")
    val replaced = saves.entries.sumOf { (t, ss) ->
      val title = child(udata, t)
      ss.count { child(title, it).isDirectory }
    }
    return Plan(kind, saves, titles, names, replaced, files, skipped, eeprom.takeIf { kind == "backup" })
  }

  /** The zip in input imported into E: (partition 1's folder); each of its saves replaces the disk's. */
  fun extract(input: InputStream, e: File): Result {
    val root = e.canonicalPath + File.separator
    val cleared = HashMap<String, File>()   // "<title>/<save>" to the save's folder, emptied once
    val dirTimes = HashMap<File, Long>()
    val skipped = ArrayList<String>()
    val written = ArrayList<File>()
    entries(input, {}) { z, en, p ->
      if (p == null) {
        if (!en.isDirectory) skipped += en.name
        return@entries
      }
      var dir = child(child(e, p.area), p.title)
      p.save?.let { s ->
        val key = "${p.title}/${s.lowercase()}"
        if (key !in cleared) {
          child(dir, s).deleteRecursively()
          cleared[key] = File(dir, s)
        }
      }
      val path = if (p.dir) p.rest else p.rest.dropLast(1)
      for (seg in path) dir = child(dir, seg)
      require((dir.canonicalPath + File.separator).startsWith(root)) { "${en.name}: outside E:" }
      dir.mkdirs()
      if (p.dir) {
        if (en.time != -1L) dirTimes[dir] = en.time
        return@entries
      }
      val f = child(dir, p.rest.last())
      f.outputStream().use { z.copyTo(it) }
      if (en.time != -1L) f.setLastModified(en.time)
      written += f
    }
    // a save's date is its folder's: the zip's, else its newest file's
    for (save in cleared.values) {
      val t = dirTimes[save] ?: save.walkTopDown().filter { it.isFile }.maxOfOrNull { it.lastModified() } ?: continue
      save.setLastModified(t)
    }
    for ((d, t) in dirTimes) if (d !in cleared.values) d.setLastModified(t)
    return Result(written, skipped)
  }
}
