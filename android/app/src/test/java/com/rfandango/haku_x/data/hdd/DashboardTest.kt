package com.rfandango.haku_x.data.hdd

import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.file.Files
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Check which dashboard files count for Insignia account setup. */
class DashboardTest {
  /** An XBE header whose certificate has this title. */
  private fun xbe(title: String): ByteArray {
    val b = ByteBuffer.allocate(0x400).order(ByteOrder.LITTLE_ENDIAN)
    b.put(0, "XBEH".toByteArray())
    b.putInt(0x104, 0x10000)
    b.putInt(0x118, 0x10180)
    title.toByteArray(Charsets.UTF_16LE).copyInto(b.array(), 0x180 + 0xC)
    return b.array()
  }

  @Test
  fun onlyTheUsersOwnCounts() {
    val root = Files.createTempDirectory("dash").toFile()
    try {
      val c = Dashboard.cDrive(root).apply { mkdirs() }
      assertFalse("an empty C:", Dashboard.installed(root))

      File(c, "xboxdash.xbe").writeBytes(xbe("hakux-dash"))
      assertFalse("hakuX-lr's dashboard", Dashboard.installed(root))

      File(c, "xboxdash.xbe").writeBytes(xbe("hakux-dash2"))
      assertTrue("a longer title starting the same", Dashboard.installed(root))

      File(c, "xboxdash.xbe").delete()
      File(c, "XBOXDASH.XBE").writeBytes(xbe("Xbox Dashboard"))
      assertTrue("the user's, any case", Dashboard.installed(root))

      File(c, "XBOXDASH.XBE").writeBytes(ByteArray(16))
      assertTrue("too short to read: not ours", Dashboard.installed(root))
    } finally {
      root.deleteRecursively()
    }
  }
}
