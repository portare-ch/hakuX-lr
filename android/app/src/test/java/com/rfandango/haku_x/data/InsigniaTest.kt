package com.rfandango.haku_x.data

import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class InsigniaTest {
  private fun put32(b: ByteArray, off: Int, v: Long) {
    for (i in 0 until 4) b[off + i] = (v ushr (8 * i)).toByte()
  }

  private fun get32(b: ByteArray, off: Int) = (0 until 4).fold(0L) { a, i -> a or ((b[off + i].toLong() and 0xFF) shl (8 * i)) }

  /** A config sector as the console writes one: signature, header, payload, footer and checksum. */
  private fun sector(area: ByteArray, n: Int, payload: (ByteArray) -> Unit) {
    val s = ByteArray(512)
    put32(s, 0, 0x79132568L); put32(s, 4, 1); put32(s, 8, 1)
    payload(s)
    put32(s, 508, 0xAA550000L)
    var sum = 0L
    for (i in 0 until 512 step 4) if (i != 504) sum += get32(s, i)
    while (sum ushr 32 != 0L) sum = (sum and 0xFFFFFFFFL) + (sum ushr 32)
    put32(s, 504, sum.inv() and 0xFFFFFFFFL)
    s.copyInto(area, n * 512)
  }

  private fun account(area: ByteArray, n: Int, gamertag: String, domain: String) = sector(area, n) { s ->
    gamertag.toByteArray().copyInto(s, 0x18)
    domain.toByteArray().copyInto(s, 0x30)
  }

  /** The user table: slots in use, each a DWORD flag and an IV. */
  private fun table(area: ByteArray, vararg slots: Int) = sector(area, 11) { s ->
    for (slot in slots) put32(s, 12 + slot * 12, 1)
  }

  private fun withHdd(area: ByteArray?, check: (File) -> Unit) {
    val root = Files.createTempDirectory("insignia").toFile()
    try {
      if (area != null) File(root, "raw-partition0.bin").writeBytes(area)
      check(root)
    } finally {
      root.deleteRecursively()
    }
  }

  @Test
  fun freshConsole() = withHdd(null) { root ->
    assertFalse(Insignia.registered(root))
    assertEquals(emptyList<String>(), Insignia.accounts(root))
  }

  @Test
  fun registeredWithAccounts() {
    val area = ByteArray(20 * 512)
    account(area, 9, "SN.012345678901", "insignia.live")   // the machine credential, not an account
    table(area, 0, 2, 3)
    account(area, 12, "hakuX Tester", "insignia.live")
    account(area, 13, "Not In Table", "insignia.live")     // its slot isn't in use
    account(area, 14, "Elsewhere", "xboxlive.com")
    account(area, 15, "Second", "INSIGNIA.LIVE")
    withHdd(area) { root ->
      assertTrue(Insignia.registered(root))
      assertEquals(listOf("hakuX Tester", "Second"), Insignia.accounts(root))
    }
  }

  @Test
  fun damagedSectorsDontCount() {
    val area = ByteArray(20 * 512)
    account(area, 9, "SN.012345678901", "insignia.live")
    table(area, 0, 1)
    account(area, 12, "Good", "insignia.live")
    account(area, 13, "Bad Sum", "insignia.live")
    area[13 * 512 + 0x40] = 1                              // after its checksum was made
    area[9 * 512 + 504] = (area[9 * 512 + 504] + 1).toByte()
    withHdd(area) { root ->
      assertFalse(Insignia.registered(root))
      assertEquals(listOf("Good"), Insignia.accounts(root))
    }
    withHdd(area.copyOf(11 * 512 + 100)) { root ->            // cut inside the table
      assertEquals(emptyList<String>(), Insignia.accounts(root))
    }
  }
}
