package com.rfandango.haku_x.data

import android.content.Context
import com.rfandango.haku_x.data.hdd.XboxHdd
import java.io.File

/**
 * Insignia, the Xbox Live revival (insignia.live): what its setup wizard (ui/insignia) checks. hakuX-lr's
 * dashboard registers the console and makes its accounts; both go in the hard disk's config area, which
 * the native core keeps as raw-partition0.bin.
 */
object Insignia {
  const val SITE = "https://insignia.live"

  private const val SECTOR = 512
  private const val MACHINE_SECTOR = 9
  private const val USER_TABLE_SECTOR = 11
  private const val FIRST_USER_SECTOR = 12
  private const val USER_SLOTS = 8

  /**
   * An earlier version downloaded Insignia's Setup Assistant here; the dashboard registers the console
   * now, so the ISO only takes space.
   */
  fun forgetSetupAssistant(context: Context) {
    File(context.filesDir, "insignia").takeIf { it.exists() }?.deleteRecursively()
  }

  /** The console is registered: the config area holds its machine credential (sector 9). */
  fun registered(hddRoot: File): Boolean {
    val d = configArea(hddRoot) ?: return false
    return validSector(d, MACHINE_SECTOR)
  }

  /**
   * The Insignia accounts on the console, their gamertags: the user table (sector 11) names the slots in
   * use, each an account sector from 12 with its gamertag at +0x18 and its domain at +0x30 ("insignia.live").
   * The machine credential and slots the table doesn't name are not accounts.
   */
  fun accounts(hddRoot: File): List<String> {
    val d = configArea(hddRoot) ?: return emptyList()
    if (!validSector(d, USER_TABLE_SECTOR)) return emptyList()
    val out = mutableListOf<String>()
    for (slot in 0 until USER_SLOTS) {
      // each entry: a DWORD in-use flag, then the slot's eight-byte IV
      if (le32(d, USER_TABLE_SECTOR * SECTOR + 12 + slot * 12) == 0L) continue
      val s = FIRST_USER_SECTOR + slot
      if (!validSector(d, s)) continue
      val name = cString(d, s * SECTOR + 0x18, 16)
      val domain = cString(d, s * SECTOR + 0x30, 20)
      if (domain.equals("insignia.live", ignoreCase = true) && name.isNotEmpty() && name.all { it in ' '..'~' }) {
        out += name
      }
    }
    return out
  }

  private fun configArea(hddRoot: File): ByteArray? {
    val f = XboxHdd.rawHead(hddRoot, 0)
    if (!f.isFile) return null
    return runCatching { f.readBytes() }.getOrNull()
  }

  /**
   * A config sector the console wrote: its signature, two nonzero header DWORDs, the 0xAA550000 footer and
   * the checksum at +504 (the complement of the end-around-carry sum of its other DWORDs).
   */
  private fun validSector(d: ByteArray, sector: Int): Boolean {
    val o = sector * SECTOR
    if (o + SECTOR > d.size) return false
    if (le32(d, o) != 0x79132568L || le32(d, o + 4) == 0L || le32(d, o + 8) == 0L || le32(d, o + 508) != 0xAA550000L) {
      return false
    }
    var sum = 0L
    for (i in 0 until SECTOR step 4) if (i != 504) sum += le32(d, o + i)
    while (sum ushr 32 != 0L) sum = (sum and 0xFFFFFFFFL) + (sum ushr 32)
    return le32(d, o + 504) == (sum.inv() and 0xFFFFFFFFL)
  }

  private fun le32(d: ByteArray, off: Int): Long =
    (d[off].toLong() and 0xFF) or ((d[off + 1].toLong() and 0xFF) shl 8) or
      ((d[off + 2].toLong() and 0xFF) shl 16) or ((d[off + 3].toLong() and 0xFF) shl 24)

  private fun cString(d: ByteArray, off: Int, max: Int): String {
    var n = 0
    while (n < max && off + n < d.size && d[off + n] != 0.toByte()) n++
    return String(d, off, n, Charsets.US_ASCII)
  }
}
