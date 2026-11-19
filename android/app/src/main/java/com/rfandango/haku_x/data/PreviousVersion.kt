package com.rfandango.haku_x.data

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File

/**
 * Migrate files from hakuX 0.3.x, which used xemu. Reuse the games folder and GPU driver.
 * Import the Xbox disk into the native core and re-sign its saves using source keys from the
 * old setup. Keep its BIOS and EEPROM as console files.
 *
 * Look for paths in xemu.toml's [sys.files], then the old x1box/ setup paths (mcpxPath,
 * flashPath, hddPath). The disk can also come from the saved hddUri document.
 */
class PreviousVersion(private val context: Context) {
  /** A disk image: one of the app's files, or a document. */
  class Image(val file: File?, val uri: Uri?, val size: Long)

  class Found(val hdd: Image, val mcpx: File?, val flash: File?, val eeprom: File?)

  private val x1box = listOfNotNull(context.getExternalFilesDir(null), context.filesDir).map { File(it, "x1box") }

  /** What the previous release left, or null: nothing of it here (a new install), or its disk gone. */
  fun find(): Found? {
    val toml = x1box.map { File(it, "xemu.toml") }.firstOrNull { it.isFile }
      ?.let { runCatching { sysFiles(it.readText()) }.getOrNull() } ?: emptyMap()
    val sp = context.getSharedPreferences(AppPrefs.NAME, Context.MODE_PRIVATE)
    fun local(tomlKey: String, prefKey: String?, name: String): File? =
      listOfNotNull(toml[tomlKey], prefKey?.let { sp.getString(it, null) }).map(::File).firstOrNull { it.isFile }
        ?: x1box.map { File(it, name) }.firstOrNull { it.isFile }

    val hdd = local("hdd_path", "hddPath", "hdd.img")?.let { Image(it, null, it.length()) }
      ?: sp.getString("hddUri", null)?.let(Uri::parse)?.takeIf { Storage.hasReadPermission(context, it) }
        ?.let { Image(null, it, size(it)) }
      ?: return null
    return Found(hdd, local("bootrom_path", "mcpxPath", "mcpx.bin"), local("flashrom_path", "flashPath", "flash.bin"),
                 local("eeprom_path", null, "eeprom.bin"))
  }

  /** The previous release's disk image once brought over, for the hard disk screen to offer deleting. */
  fun leftoverImage(): File? = find()?.hdd?.file?.takeIf { f -> x1box.any { f.parentFile == it } }

  private fun size(uri: Uri): Long = runCatching {
    context.contentResolver.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)?.use { c ->
      if (c.moveToFirst()) c.getLong(0) else 0L
    }
  }.getOrNull() ?: 0L

  companion object {
    /** The paths an xemu.toml's [sys.files] section gives (bootrom_path, flashrom_path, eeprom_path, hdd_path). */
    fun sysFiles(toml: String): Map<String, String> {
      val out = HashMap<String, String>()
      var inSection = false
      for (raw in toml.lineSequence()) {
        val line = raw.trim()
        if (line.startsWith("[")) {
          inSection = line == "[sys.files]"
          continue
        }
        if (!inSection) continue
        val m = Regex("""^([A-Za-z_]+)\s*=\s*(['"])(.*)\2\s*$""").find(line) ?: continue
        out[m.groupValues[1]] = m.groupValues[3]
      }
      return out
    }
  }
}
