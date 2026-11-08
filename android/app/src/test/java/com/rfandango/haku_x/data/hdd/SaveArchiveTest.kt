package com.rfandango.haku_x.data.hdd

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Files
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class SaveArchiveTest {
  private fun file(f: File, text: String, mtime: Long = 1_700_000_000_000L) {
    f.parentFile!!.mkdirs()
    f.writeText(text)
    f.setLastModified(mtime)
  }

  /** The zip's entries: name to (contents or null for a folder, time). */
  private fun entries(zip: ByteArray): Map<String, Pair<ByteArray?, Long>> {
    val out = LinkedHashMap<String, Pair<ByteArray?, Long>>()
    ZipInputStream(ByteArrayInputStream(zip)).use { z ->
      while (true) {
        val e = z.nextEntry ?: break
        out[e.name] = (if (e.isDirectory) null else z.readBytes()) to e.time / 2000
      }
    }
    return out
  }

  @Test
  fun layout() {
    val udata = Files.createTempDirectory("udata").toFile()
    try {
      val halo = File(udata, "4d530004")
      file(File(halo, "TitleMeta.xbx"), "TitleName=Halo")
      file(File(halo, "TitleImage.xbx"), "image")
      file(File(halo, "a1/SaveMeta.xbx"), "Name=One")
      file(File(halo, "a1/blam.sav"), "one", 1_600_000_000_000L)
      file(File(halo, "a2/SaveMeta.xbx"), "Name=Two")
      file(File(halo, "a2/sub/deep.bin"), "two")
      file(File(udata, "5345000a/b1/jsrf.sav"), "jsrf")

      // one save: the title's files and that save only
      val one = ByteArrayOutputStream()
      assertEquals(4, SaveArchive.write(one, listOf(SaveArchive.Pick(halo, listOf(File(halo, "a1"))))))
      val e = entries(one.toByteArray())
      assertEquals(
        listOf(Backup.MARKER, "UDATA/4d530004/", "UDATA/4d530004/TitleImage.xbx", "UDATA/4d530004/TitleMeta.xbx",
               "UDATA/4d530004/a1/", "UDATA/4d530004/a1/SaveMeta.xbx", "UDATA/4d530004/a1/blam.sav"),
        e.keys.toList())
      assertArrayEquals("one".toByteArray(), e["UDATA/4d530004/a1/blam.sav"]!!.first)
      assertEquals(1_600_000_000_000L / 2000, e["UDATA/4d530004/a1/blam.sav"]!!.second)

      // every title, every save
      val all = ByteArrayOutputStream()
      val titles = udata.listFiles()!!.sortedBy { it.name }.map { SaveArchive.Pick(it) }
      assertEquals(7, SaveArchive.write(all, titles))
      val a = entries(all.toByteArray())
      assertArrayEquals("two".toByteArray(), a["UDATA/4d530004/a2/sub/deep.bin"]!!.first)
      assertArrayEquals("jsrf".toByteArray(), a["UDATA/5345000a/b1/jsrf.sav"]!!.first)
    } finally {
      udata.deleteRecursively()
    }
  }

  private fun zip(vararg entries: Pair<String, String?>): ByteArray {
    val b = ByteArrayOutputStream()
    ZipOutputStream(b).use { z ->
      for ((name, text) in entries) {
        z.putNextEntry(ZipEntry(name).apply { time = 1_650_000_000_000L })
        // .xbx metadata is UTF-16, as the Xbox writes it
        text?.let { z.write(it.toByteArray(if (name.endsWith(".xbx")) Charsets.UTF_16LE else Charsets.UTF_8)) }
        z.closeEntry()
      }
    }
    return b.toByteArray()
  }

  private fun tree(dir: File) = dir.walkTopDown().filter { it.isFile }
    .associate { it.relativeTo(dir).path.replace('\\', '/') to it.readText() }

  @Test
  fun roundTrip() {
    val tmp = Files.createTempDirectory("saves").toFile()
    try {
      val a = File(tmp, "a/UDATA/4d530004")
      file(File(a, "TitleMeta.xbx"), "TitleName=Halo")
      file(File(a, "a1/SaveMeta.xbx"), "Name=One")
      file(File(a, "a1/blam.sav"), "one", 1_600_000_000_000L)
      val out = ByteArrayOutputStream()
      SaveArchive.write(out, listOf(SaveArchive.Pick(a)))
      val e = File(tmp, "e")
      val plan = SaveArchive.plan(ByteArrayInputStream(out.toByteArray()), e)
      assertEquals(mapOf("4d530004" to setOf("a1")), plan.saves)
      assertEquals(0, plan.replaced)
      val r = SaveArchive.extract(ByteArrayInputStream(out.toByteArray()), e)
      assertEquals(3, r.files)
      assertEquals(tree(File(tmp, "a")), tree(e))
      assertEquals(1_600_000_000_000L / 2000, File(e, "UDATA/4d530004/a1/blam.sav").lastModified() / 2000)
    } finally {
      tmp.deleteRecursively()
    }
  }

  @Test
  fun importVariants() {
    val e = Files.createTempDirectory("e").toFile()
    try {
      // the disk: a save the zip replaces (another spelling of its folder) and one it leaves
      file(File(e, "UDATA/4d530064/abc/old.bin"), "old")
      file(File(e, "UDATA/4d530064/keep/k.bin"), "keep")
      val z = zip(
        "Halo 2 saves/4D530064/TitleMeta.xbx" to "TitleName=Halo 2",   // no UDATA, in a folder, upper case
        "Halo 2 saves/4D530064/ABC/" to null,
        "Halo 2 saves/4D530064/ABC/new.bin" to "new",
        "Halo 2 saves/TDATA/4D530064/settings.bin" to "settings",
        "readme.txt" to "hi",
        "UDATA/4d530064/../../evil.bin" to "evil",
      )
      val plan = SaveArchive.plan(ByteArrayInputStream(z), e)
      assertEquals(mapOf("4d530064" to setOf("abc")), plan.saves)
      assertEquals("Halo 2", plan.names["4d530064"])
      assertEquals(1, plan.replaced)
      assertEquals(3, plan.files)
      assertEquals(listOf("readme.txt", "UDATA/4d530064/../../evil.bin"), plan.skipped)

      val r = SaveArchive.extract(ByteArrayInputStream(z), e)
      assertEquals(3, r.files)
      assertEquals(
        mapOf("UDATA/4d530064/TitleMeta.xbx" to String("TitleName=Halo 2".toByteArray(Charsets.UTF_16LE)),
              "UDATA/4d530064/ABC/new.bin" to "new",
              "UDATA/4d530064/keep/k.bin" to "keep", "TDATA/4d530064/settings.bin" to "settings"),
        tree(e))
      assertEquals(1_650_000_000_000L / 2000, File(e, "UDATA/4d530064/ABC").lastModified() / 2000)
      assertFalse(File(e.parentFile, "evil.bin").exists())
    } finally {
      e.deleteRecursively()
    }
  }

  /** Sign a console-locked save with a zero title key and the supplied HDD key. */
  private fun locked(d: ByteArray, hd: ByteArray) = d + XCrypto.hmac(hd, XCrypto.hmac(ByteArray(16), d))

  @Test
  fun transferBetweenConsoles() {
    val tmp = Files.createTempDirectory("transfer").toFile()
    try {
      val hdA = ByteArray(16) { (it * 7 + 1).toByte() }
      val hdB = ByteArray(16) { (it * 11 + 3).toByte() }
      val data = ByteArray(200) { (it * 3).toByte() }
      val title = File(tmp, "a/UDATA/4d530004")
      file(File(title, "TitleMeta.xbx"), "TitleName=Halo")
      val sav = File(title, "a1/blam.sav").apply { parentFile!!.mkdirs(); writeBytes(locked(data, hdA)) }

      // Export re-signs staged copies with the legacy key.
      val stage = File(tmp, "stage")
      val picks = SaveArchive.portable(listOf(SaveArchive.Pick(title)), hdA, stage)
      assertArrayEquals(locked(data, hdA), sav.readBytes())
      assertArrayEquals(locked(data, SaveResigner.LEGACY_HD), File(picks[0].title, "a1/blam.sav").readBytes())
      val zip = ByteArrayOutputStream()
      SaveArchive.write(zip, picks)
      // Saves already signed with the legacy key can skip staging.
      val same = listOf(SaveArchive.Pick(title))
      assertEquals(same, SaveArchive.portable(same, SaveResigner.LEGACY_HD.copyOf(), File(tmp, "unused")))
      assertFalse(File(tmp, "unused").exists())

      // Import re-signs the extracted saves with the destination key.
      val e = File(tmp, "b")
      val plan = SaveArchive.plan(ByteArrayInputStream(zip.toByteArray()), e)
      assertEquals("saves", plan.kind)
      assertEquals(null, plan.eeprom)
      val r = SaveArchive.extract(ByteArrayInputStream(zip.toByteArray()), e)
      SaveResigner.hakux(SaveResigner.LEGACY_HD, hdB).resign(e, r.written) { null }
      assertArrayEquals(locked(data, hdB), File(e, "UDATA/4d530004/a1/blam.sav").readBytes())
    } finally {
      tmp.deleteRecursively()
    }
  }

  @Test
  fun backupGivesItsKey() {
    val eeprom = ByteArray(256)
    val key = ByteArray(16) { (0x40 + it).toByte() }
    key.copyInto(eeprom, 0x36)                // an image from before: its section in the clear
    val b = ByteArrayOutputStream()
    ZipOutputStream(b).use { z ->
      Backup.putMarker(z, Backup.Info("backup", 1_650_000_000_000L, states = false, caches = false))
      z.putNextEntry(ZipEntry("hdd/Partition1/UDATA/4d530004/a1/blam.sav"))
      z.write(byteArrayOf(1, 2, 3))
      z.closeEntry()
      z.putNextEntry(ZipEntry("hdd/eeprom.bin"))
      z.write(eeprom)
      z.closeEntry()
    }
    val plan = SaveArchive.plan(ByteArrayInputStream(b.toByteArray()), File("/nonexistent"))
    assertEquals("backup", plan.kind)
    assertArrayEquals(key, SaveResigner.hdKeyOf(plan.eeprom!!))
    assertEquals(mapOf("4d530004" to setOf("a1")), plan.saves)
  }
}
