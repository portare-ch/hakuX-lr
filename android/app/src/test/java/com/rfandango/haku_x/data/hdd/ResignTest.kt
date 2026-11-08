package com.rfandango.haku_x.data.hdd

import java.io.File
import java.nio.file.Files
import kotlin.random.Random
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** Key extraction and re-signing, with made-up keys (the real ones come from the user's dumps). */
class ResignTest {
  private val rnd = Random(7)
  private fun bytes(n: Int) = ByteArray(n) { rnd.nextInt().toByte() }
  private val zero = ByteArray(16)

  private val eepromKey = bytes(16)
  private val mid = listOf(ConsoleKeys.Midstates("test", XCrypto.innerMid(eepromKey), XCrypto.outerMid(eepromKey)))

  @Test
  fun midstateHmac() {
    val msg = bytes(77)
    assertArrayEquals(XCrypto.hmac(eepromKey, msg), XCrypto.hmacMid(mid[0].inner, mid[0].outer, msg))
    val h = XCrypto.Hmac(eepromKey)
    h.update(msg, 0, 30)
    val fork = h.copy()
    h.update(msg, 30, 47)
    assertArrayEquals(XCrypto.hmac(eepromKey, msg), h.finish())
    assertArrayEquals(XCrypto.hmac(eepromKey, msg.copyOf(30)), fork.finish())
  }

  @Test
  fun certKeyFromBios() {
    val cert = bytes(16)
    val bl = bytes(0x6000)
    eepromKey.copyInto(bl, 0x1234)
    cert.copyInto(bl, 0x1244)
    val mcpx = bytes(512)
    val flash = bytes(0x40000)
    XCrypto.rc4(mcpx.copyOfRange(0x1A5, 0x1B5), bl).copyInto(flash, flash.size - 0x6200)
    assertArrayEquals(cert, ConsoleKeys.certKey(mcpx, flash, mid))
    // a 1 MiB image (the 256 KiB one four times) reads the same
    val mirrored = flash + flash + flash + flash
    assertArrayEquals(cert, ConsoleKeys.certKey(mcpx, mirrored, mid))
    assertNull(ConsoleKeys.certKey(bytes(512), flash, mid))   // another boot ROM
  }

  @Test
  fun hdKeyFromEeprom() {
    val hd = bytes(16)
    val plain = bytes(8) + hd + byteArrayOf(1, 0, 0, 0)
    val hash = XCrypto.hmac(eepromKey, plain)
    val eeprom = hash + XCrypto.rc4(XCrypto.hmac(eepromKey, hash), plain) + bytes(256 - 48)
    assertArrayEquals(hd, ConsoleKeys.hdKey(eeprom, mid))
    eeprom[30] = (eeprom[30] + 1).toByte()
    assertNull(ConsoleKeys.hdKey(eeprom, mid))
    assertNull(ConsoleKeys.hdKey(ByteArray(256), mid))   // hakuX's own EEPROM has no encrypted section
  }

  @Test
  fun resign() {
    val tmp = Files.createTempDirectory("resign").toFile()
    try {
      val certKey = bytes(16)
      val hd = bytes(16)
      val native = bytes(16)                                                  // the native kernel's HD key
      val r = SaveResigner(certKey, hd, native)
      val titleKey = bytes(16)
      val auth = r.authKey(titleKey)
      fun roam(k: ByteArray, d: ByteArray) = XCrypto.hmac(k, d)
      fun noroam(k: ByteArray, h: ByteArray, d: ByteArray) = XCrypto.hmac(h, XCrypto.hmac(k, d))

      val e = File(tmp, "Partition1")
      val save = File(e, "UDATA/4d530064/ABCDEF012345").apply { mkdirs() }
      val d1 = bytes(100)
      val d2 = bytes(5000)
      val d3 = bytes(64)
      val d4 = bytes(333)
      val d5 = bytes(300)
      val hakux = bytes(80)
      fun put(name: String, b: ByteArray) = File(save, name).apply { writeBytes(b); setLastModified(1_600_000_000_000L) }
      val footer = put("footer.bin", d1 + roam(auth, d1))
      val header = put("header.bin", roam(auth, d2) + d2)
      val at4 = put("at4.bin", byteArrayOf(1, 2, 3, 4) + roam(auth, d3) + d3)
      val locked = put("locked.bin", d4 + noroam(auth, hd, d4))
      val diary = put("diary", d5)
      val diarySig = put("diary.sig", roam(auth, d5))
      val plain = put("plain.txt", bytes(50))
      val ours = put("ours.bin", hakux + roam(zero, hakux))                 // already hakuX's: left as it is
      // a .sig file holding two files' signatures, signed itself over them (Forza's CarIcons.sig)
      val i1 = bytes(700)
      val i2 = bytes(900)
      val icon1 = put("icon1.xpr", i1)
      val icon2 = put("icon2.xpr", i2)
      val slots = noroam(auth, hd, i1) + noroam(auth, hd, i2)
      val iconSig = put("icons.sig", slots + noroam(auth, hd, slots))
      // its data and signature, padded to a fixed size (Fable's Profile.bin)
      val d6 = bytes(500)
      val pad = ByteArray(4096 - 520) { 0x20 }
      val padded = put("padded.bin", d6 + noroam(auth, hd, d6) + pad)
      // a game's downloaded content: its header signed with HMAC(HMAC(HD key, title id), header)
      val cm = bytes(0x80)
      cm[0x18] = 0x60; cm[0x19] = 0; cm[0x1A] = 0; cm[0x1B] = 0
      byteArrayOf(0x64, 0x00, 0x53, 0x4D).copyInto(cm, 0x24)
      val tid = cm.copyOfRange(0x24, 0x28)
      XCrypto.hmac(XCrypto.hmac(hd, tid), cm, 0x14, 0x60 - 0x14).copyInto(cm, 0)
      val content = File(e, "TDATA/4d530064/\$c/00000001/ContentMeta.xbx").apply { parentFile!!.mkdirs(); writeBytes(cm) }
      // a title no game in the library has
      val other = File(e, "UDATA/41560001/111111111111/x.sav").apply { parentFile!!.mkdirs(); writeBytes(d1 + roam(auth, d1)) }

      val rep = r.resignAll(e) { id -> if (id == 0x4d530064L) titleKey else null }
      assertEquals(10, rep.resigned)
      assertEquals(listOf("4d530064/ABCDEF012345/ours.bin", "4d530064/ABCDEF012345/plain.txt"), rep.unsigned.sorted())
      assertEquals(setOf("41560001"), rep.noKey)

      assertArrayEquals(d1 + roam(zero, d1), footer.readBytes())
      assertArrayEquals(roam(zero, d2) + d2, header.readBytes())
      assertArrayEquals(byteArrayOf(1, 2, 3, 4) + roam(zero, d3) + d3, at4.readBytes())
      assertArrayEquals(d4 + noroam(zero, native, d4), locked.readBytes())
      assertArrayEquals(d5, diary.readBytes())
      assertArrayEquals(roam(zero, d5), diarySig.readBytes())
      assertArrayEquals(hakux + roam(zero, hakux), ours.readBytes())
      val newSlots = noroam(zero, native, i1) + noroam(zero, native, i2)
      assertArrayEquals(newSlots + noroam(zero, native, newSlots), iconSig.readBytes())
      assertArrayEquals(d6 + noroam(zero, native, d6) + pad, padded.readBytes())
      assertArrayEquals(i1, icon1.readBytes())
      assertArrayEquals(i2, icon2.readBytes())
      assertArrayEquals(XCrypto.hmac(XCrypto.hmac(native, tid), cm, 0x14, 0x60 - 0x14), content.readBytes().copyOf(20))
      assertArrayEquals(d1 + roam(auth, d1), other.readBytes())
      assertEquals(1_600_000_000_000L / 1000, footer.lastModified() / 1000)
      assertTrue(plain.isFile)
    } finally {
      tmp.deleteRecursively()
    }
  }

  @Test
  fun hakuxToHakux() {
    val tmp = Files.createTempDirectory("resign-hakux").toFile()
    try {
      val from = bytes(16)
      val to = bytes(16)
      fun roam(d: ByteArray) = XCrypto.hmac(zero, d)
      val e = File(tmp, "Partition1")
      val save = File(e, "UDATA/4d530064/ABCDEF012345").apply { mkdirs() }
      val d1 = bytes(120)
      val d2 = bytes(90)
      val locked = File(save, "locked.bin").apply { writeBytes(d1 + XCrypto.hmac(from, roam(d1))) }
      val roaming = File(save, "roam.bin").apply { writeBytes(d2 + roam(d2)) }
      val cm = bytes(0x80)
      cm[0x18] = 0x60; cm[0x19] = 0; cm[0x1A] = 0; cm[0x1B] = 0
      byteArrayOf(0x64, 0x00, 0x53, 0x4D).copyInto(cm, 0x24)
      val tid = cm.copyOfRange(0x24, 0x28)
      XCrypto.hmac(XCrypto.hmac(from, tid), cm, 0x14, 0x60 - 0x14).copyInto(cm, 0)
      val content = File(e, "TDATA/4d530064/\$c/00000001/ContentMeta.xbx").apply { parentFile!!.mkdirs(); writeBytes(cm) }

      // hakuX uses zero title keys, so the lookup can return null.
      val rep = SaveResigner.hakux(from, to).resign(e, e.walkTopDown().filter { it.isFile }.toList()) { null }
      assertEquals(3, rep.resigned)
      assertTrue(rep.noKey.isEmpty())
      assertArrayEquals(d1 + XCrypto.hmac(to, roam(d1)), locked.readBytes())
      assertArrayEquals(d2 + roam(d2), roaming.readBytes())
      assertArrayEquals(XCrypto.hmac(XCrypto.hmac(to, tid), cm, 0x14, 0x60 - 0x14), content.readBytes().copyOf(20))
    } finally {
      tmp.deleteRecursively()
    }
  }
}
