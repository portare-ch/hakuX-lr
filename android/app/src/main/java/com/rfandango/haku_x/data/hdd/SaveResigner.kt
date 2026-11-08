package com.rfandango.haku_x.data.hdd

import java.io.File
import java.io.RandomAccessFile

/**
 * Re-signs a console's or xemu's saves for hakuX. Games sign with XAPI's XCalculateSignature (as
 * feudalnate's port of it reads): roamable = HMAC-SHA1(title key, data), the title key being
 * HMAC-SHA1(certificate key, the certificate's signature key)[0:16]; locked to the console =
 * HMAC-SHA1(HD key, roamable). Downloaded content's ContentMeta.xbx: HMAC-SHA1(HMAC-SHA1(HD key, title
 * id), its header). hakuX's kernel signs as a console whose title keys are zero and whose HD key is
 * its EEPROM's bytes 0x36-0x45 (nativeHd: native/kernel/ex.c nc_kernel_set_hd_key).
 *
 * Transfers between hakuX consoles need only the HDD keys because both use zero title keys.
 *
 * A game puts the signature where it likes. The places XSavSig's resign.ini lists for most games are
 * tried (the signature at the start, at 4, or at the end, the end also before padding, over the bytes
 * around it, or in a .sig file of its own), and a signature is replaced only when it checks with the
 * source keys: a file whose signature isn't found is left as it is. A folder's signatures are all
 * found before any is written:
 * a .sig file may sign itself too, over the others' signatures (Forza's CarIcons.sig), and its own
 * comes after theirs.
 */
class SaveResigner(private val certKey: ByteArray?, private val hdKey: ByteArray, private val nativeHd: ByteArray) {
  class Report(
    /** Files whose signature was replaced. */
    val resigned: Int,
    /** Files without a signature found (unsigned data, or a layout not tried), as "<title>/<path>". */
    val unsigned: List<String>,
    /** Titles whose game isn't in the library: their saves stay as they are. */
    val noKey: Set<String>,
  )

  fun authKey(titleKey: ByteArray): ByteArray = XCrypto.hmac(certKey!!, titleKey).copyOf(16)

  /** Every file of E:'s titles (UDATA, TDATA) re-signed: a console's whole disk. */
  fun resignAll(e: File, titleKey: (Long) -> ByteArray?): Report =
    resign(e, listOf("UDATA", "TDATA").flatMap { area ->
      e.listFiles().orEmpty().filter { it.isDirectory && it.name.equals(area, ignoreCase = true) }
        .flatMap { it.walkTopDown().filter(File::isFile).toList() }
    }, titleKey)

  /**
   * These files of E: (partition 1's folder) re-signed, each with its title's key (UDATA or TDATA
   * \<title id>\...). titleKey: a title id's signature key from its game's certificate
   * ([XboxDisc.saveKeys]). Return null for missing keys. hakuX transfers skip this lookup.
   */
  fun resign(e: File, files: List<File>, titleKey: (Long) -> ByteArray?): Report {
    var contents = 0
    val found = ArrayList<Pair<File, Hit>>()
    val unsigned = ArrayList<String>()
    val noKey = LinkedHashSet<String>()
    val auths = HashMap<Long, ByteArray?>()
    // a file of 20 bytes or less is no signed data (a .sig file's signature is found from its data's)
    for (f in files.filter { it.isFile && it.length() in 21L..MAX_FILE && !isMeta(it.name) }) {
      val parts = f.relativeTo(e).path.replace('\\', '/').split('/')
      if (parts.size < 3) continue
      val id = parts[1].lowercase()
      val tid = id.toLongOrNull(16) ?: continue
      val where = "$id/${parts.drop(2).joinToString("/")}"
      if (f.name.equals(CONTENT_META, ignoreCase = true)) {
        if (content(f, f.readBytes())) contents++ else unsigned += where
        continue
      }
      val auth = auths.getOrPut(tid) { if (certKey == null) ZERO else titleKey(tid)?.let(::authKey) }
      val siblings = f.parentFile!!.listFiles().orEmpty().filter { it.isFile && it != f }
      val hit = auth?.let { find(f.readBytes(), siblings, it) }
      when {
        hit != null -> found += f to hit
        auth == null -> noKey += id
        else -> unsigned += where
      }
    }
    // a file holding others' signatures signs itself last, over theirs
    val holders = found.mapNotNull { it.second.file }.toSet()
    for ((f, hit) in found.sortedBy { (f, hit) -> hit.file == null && f in holders }) {
      val data = f.readBytes()
      var sig = XCrypto.hmac(ZERO, data, hit.a, hit.b - hit.a)
      if (hit.kind == NOROAM) sig = XCrypto.hmac(nativeHd, sig)
      write(hit.file ?: f, hit.off, sig)
    }
    return Report(found.size + contents, unsigned, noKey)
  }

  /** A signature found: the data it covers [a, b), its kind, and where it is (file null: this one). */
  private class Hit(val a: Int, val b: Int, val kind: Int, val file: File?, val off: Long)

  /**
   * Where the file's signature is: its data range, kind and place, or null. The places are tried from
   * the file's end, then from its content's end, before padding (zeros or spaces): a game may write
   * its data and the signature after it into a file of a fixed size (Fable's 4 KiB Profile.bin).
   */
  private fun find(d: ByteArray, siblings: List<File>, auth: ByteArray): Hit? {
    val n = d.size
    val small = siblings.filter { it.length() in 20..4096 }.map { it to it.readBytes() }
    for (m in listOf(n, unpadded(d, 0), unpadded(d, 0x20)).filter { it > 20 }.distinct()) {
      val ends = listOf(m - 24, m - 20, m - 4, m).filter { it > 0 }.distinct()
      for (a in listOf(0, 4, 20, 24)) {
        val h = XCrypto.Hmac(auth)
        var at = a
        for (b in ends) {
          if (b <= a) continue
          h.update(d, at, b - at)
          at = b
          val roam = h.copy().finish()
          val sigs = listOf(ROAM to roam, NOROAM to XCrypto.hmac(hdKey, roam))
          for (p in listOf(0, 4, m - 24, m - 20)) {
            if (p < 0 || p + 20 > m || (p + 20 > a && p < b)) continue
            for ((kind, s) in sigs) if (matches(d, p, s)) return Hit(a, b, kind, null, p.toLong())
          }
          if (a == 0 && b == n) {
            for ((file, bytes) in small) for ((kind, s) in sigs) {
              val q = indexOf(bytes, s)
              if (q >= 0) return Hit(a, b, kind, file, q.toLong())
            }
          }
        }
      }
    }
    return null
  }

  /** d's length without the trailing run of pad bytes. */
  private fun unpadded(d: ByteArray, pad: Int): Int {
    var m = d.size
    while (m > 0 && d[m - 1].toInt() == pad) m--
    return m
  }

  /** ContentMeta.xbx: its header [0x14, header size) signed at 0 with HMAC(HD key, title id at 0x24). */
  private fun content(f: File, d: ByteArray): Boolean {
    if (d.size < 0x28) return false
    val size = le32(d, 0x18)
    if (size <= 0x14 || size > d.size) return false
    val tid = d.copyOfRange(0x24, 0x28)
    val sig = XCrypto.hmac(XCrypto.hmac(hdKey, tid), d, 0x14, size - 0x14)
    if (!matches(d, 0, sig)) return false
    write(f, 0, XCrypto.hmac(XCrypto.hmac(nativeHd, tid), d, 0x14, size - 0x14))
    return true
  }

  /** A signature written at off of f, keeping its date. */
  private fun write(f: File, off: Long, sig: ByteArray) {
    val time = f.lastModified()
    RandomAccessFile(f, "rw").use { it.seek(off); it.write(sig) }
    f.setLastModified(time)
  }

  private fun matches(d: ByteArray, p: Int, s: ByteArray): Boolean {
    for (i in 0 until 20) if (d[p + i] != s[i]) return false
    return true
  }

  private fun indexOf(hay: ByteArray, s: ByteArray): Int {
    outer@ for (i in 0..hay.size - s.size) {
      for (j in s.indices) if (hay[i + j] != s[j]) continue@outer
      return i
    }
    return -1
  }

  private fun le32(d: ByteArray, o: Int) =
    (d[o].toInt() and 0xFF) or ((d[o + 1].toInt() and 0xFF) shl 8) or ((d[o + 2].toInt() and 0xFF) shl 16) or
      ((d[o + 3].toInt() and 0xFF) shl 24)

  companion object {
    const val CONTENT_META = "ContentMeta.xbx"
    private const val ROAM = 0
    private const val NOROAM = 1
    private const val MAX_FILE = 128L shl 20
    private val ZERO = ByteArray(16)
    private val META = setOf("titlemeta.xbx", "titleimage.xbx", "savemeta.xbx", "saveimage.xbx")

    fun isMeta(name: String) = name.lowercase() in META

    /** Shared HDD key used by older installs and portable save ZIPs. */
    val LEGACY_HD: ByteArray = "0000000001".toByteArray() + byteArrayOf(0x00, 0x50, 0xF2.toByte(), 0x00, 0x00, 0x01)

    /** Re-sign saves between hakuX consoles. */
    fun hakux(from: ByteArray, to: ByteArray) = SaveResigner(null, from, to)

    /** Read the encrypted HDD key, falling back to bytes 0x36–0x45 for older EEPROM images. */
    fun hdKeyOf(eeprom: ByteArray): ByteArray? =
      ConsoleKeys.hdKey(eeprom) ?: eeprom.takeIf { it.size >= 0x46 }?.copyOfRange(0x36, 0x46)

    /** Read the console's HDD key. Imports before first boot use the legacy key the core will adopt. */
    fun nativeHdKey(hdd: File): ByteArray {
      val eeprom = runCatching { File(hdd, "eeprom.bin").takeIf { it.length() >= 0x46 }?.readBytes() }.getOrNull()
        ?: return LEGACY_HD.copyOf()
      return hdKeyOf(eeprom) ?: LEGACY_HD.copyOf()
    }
  }
}
