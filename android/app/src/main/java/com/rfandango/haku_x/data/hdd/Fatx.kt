package com.rfandango.haku_x.data.hdd

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Calendar
import java.util.TimeZone

/**
 * FATX starts with a 4 KiB superblock containing the FATX magic, volume ID, sectors per
 * cluster and root cluster. The FAT uses 16-bit entries below 65,520 clusters and 32-bit
 * entries otherwise. Entry zero holds the media byte. Data clusters start at 1.
 *
 * Each directory entry is 64 bytes. It holds the name length, attributes, 42 name bytes,
 * first cluster, size and FAT timestamps with a 2000 epoch. A length of 0x00 or 0xFF marks
 * the end and 0xE5 marks a deleted entry.
 */
object Fatx {
  const val SUPERBLOCK = 0x1000L
  const val ENTRY = 64
  const val NAME_MAX = 42
  const val ATTR_DIR = 0x10
  private const val MAGIC = 0x58544146 // "FATX"
  private const val END_OF_DIR = 0xFF
  private const val DELETED = 0xE5

  /** A file system's layout over a partition of `size` bytes. */
  class Layout(val size: Long, val sectorsPerCluster: Int) {
    val clusterSize = sectorsPerCluster * 512L
    /** Clusters in the partition (what the FAT covers, entry 0 included). */
    val clusters: Long = size / clusterSize
    val wide = clusters >= 0xFFF0
    val entrySize = if (wide) 4 else 2
    /** FAT bytes, rounded up to 4 KiB. */
    val fatSize: Long = ((clusters + 1) * entrySize + 0xFFF) and 0xFFF.inv().toLong()
    val dataStart = SUPERBLOCK + fatSize
    /** Data clusters (numbered 1..dataClusters). */
    val dataClusters: Long = (size - dataStart) / clusterSize
    val endOfChain: Long = if (wide) 0xFFFFFFFFL else 0xFFFFL
    val media: Long = if (wide) 0xFFFFFFF8L else 0xFFF8L

    fun clusterOffset(c: Long) = dataStart + (c - 1) * clusterSize
  }

  /** One file or directory. */
  class Entry(
    val name: String,
    val attributes: Int,
    val firstCluster: Long,
    val size: Long,
    val written: Long,       // unix ms
    val created: Long,
    val accessed: Long,
  ) {
    val isDirectory get() = attributes and ATTR_DIR != 0
  }

  /** Reads a FATX volume through `read(offset, buffer)` (partition offsets). */
  class Reader(private val read: (Long, ByteArray) -> Unit, partitionSize: Long) {
    val volumeId: Int
    val layout: Layout
    private val root: Long
    private val fat: ByteBuffer

    init {
      val sb = ByteArray(16)
      read(0, sb)
      val b = ByteBuffer.wrap(sb).order(ByteOrder.LITTLE_ENDIAN)
      require(b.getInt(0) == MAGIC) { "not a FATX file system" }
      volumeId = b.getInt(4)
      val spc = b.getInt(8)
      require(spc in 1..1024 && spc and (spc - 1) == 0) { "bad cluster size ($spc sectors)" }
      root = b.getInt(12).toLong() and 0xFFFFFFFFL
      layout = Layout(partitionSize, spc)
      val f = ByteArray(layout.fatSize.toInt())
      read(SUPERBLOCK, f)
      fat = ByteBuffer.wrap(f).order(ByteOrder.LITTLE_ENDIAN)
    }

    private fun next(c: Long): Long =
      if (layout.wide) fat.getInt((c * 4).toInt()).toLong() and 0xFFFFFFFFL
      else fat.getShort((c * 2).toInt()).toLong() and 0xFFFFL

    /** The clusters of a chain, in order (stops at a bad link or a loop). */
    fun chain(first: Long): List<Long> {
      val out = ArrayList<Long>()
      var c = first
      val seen = HashSet<Long>()
      while (c in 1..layout.dataClusters && seen.add(c)) {
        out += c
        val n = next(c)
        if (n == 0L || n >= (if (layout.wide) 0xFFFFFFF0L else 0xFFF0L)) break
        c = n
      }
      return out
    }

    /** A directory's entries (the root's when first is null). */
    fun list(first: Long? = null): List<Entry> {
      val out = ArrayList<Entry>()
      val buf = ByteArray(layout.clusterSize.toInt())
      for (c in chain(first ?: root)) {
        read(layout.clusterOffset(c), buf)
        val b = ByteBuffer.wrap(buf).order(ByteOrder.LITTLE_ENDIAN)
        var o = 0
        while (o + ENTRY <= buf.size) {
          val len = buf[o].toInt() and 0xFF
          if (len == END_OF_DIR || len == 0) return out
          if (len != DELETED && len <= NAME_MAX) {
            val name = String(buf, o + 2, len, Charsets.ISO_8859_1)
            out += Entry(
              name, buf[o + 1].toInt() and 0xFF, b.getInt(o + 0x2C).toLong() and 0xFFFFFFFFL,
              b.getInt(o + 0x30).toLong() and 0xFFFFFFFFL,
              fromFat(b.getInt(o + 0x34)), fromFat(b.getInt(o + 0x38)), fromFat(b.getInt(o + 0x3C)),
            )
          }
          o += ENTRY
        }
      }
      return out
    }

    /** A file's bytes in order, chunk by chunk (at most a cluster each). */
    fun readFile(e: Entry, sink: (ByteArray, Int) -> Unit) {
      var left = e.size
      val buf = ByteArray(layout.clusterSize.toInt())
      for (c in chain(e.firstCluster)) {
        if (left <= 0) break
        read(layout.clusterOffset(c), buf)
        val n = minOf(left, layout.clusterSize).toInt()
        sink(buf, n)
        left -= n
      }
    }
  }

  /**
   * Lay out the Writer.Node tree as a virtual FATX image. Allocate files and directories in
   * order, with a directory's own clusters before its children. readAt generates the
   * requested superblock, FAT, directory or file bytes on demand and returns zeros for unused
   * space.
   */
  class Writer(val layout: Layout, private val volumeId: Int, root: Node) {
    /** A file (data from source, `size` bytes) or a directory (children). */
    class Node(
      val name: String,
      val isDirectory: Boolean,
      val size: Long = 0,
      val written: Long = 0,
      val children: List<Node> = emptyList(),
      /** reads `len` bytes at `offset` of the file into buf at 0 */
      val source: ((offset: Long, buf: ByteArray, len: Int) -> Unit)? = null,
    ) {
      var first = 0L
      var clusters = 0L
    }

    private val rootNode = root
    /** Allocation in cluster order: first cluster -> its node (directory or file). */
    private val starts = java.util.TreeMap<Long, Node>()
    val usedClusters: Long

    init {
      var next = 1L
      fun alloc(n: Node) {
        val bytes = if (n.isDirectory) maxOf(1L, n.children.size + 1L) * ENTRY else n.size
        val count = if (n.isDirectory) maxOf(1L, (bytes + layout.clusterSize - 1) / layout.clusterSize)
        else (bytes + layout.clusterSize - 1) / layout.clusterSize
        n.clusters = count
        n.first = if (count > 0) next else 0
        if (count > 0) starts[next] = n
        next += count
        if (n.isDirectory) n.children.forEach(::alloc)
      }
      alloc(rootNode)
      usedClusters = next - 1
      require(usedClusters <= layout.dataClusters) {
        "the files need ${usedClusters * layout.clusterSize shr 20} MiB, the partition holds " +
          "${layout.dataClusters * layout.clusterSize shr 20} MiB"
      }
    }

    /** Bytes [offset, offset + len) of the partition into buf at 0. */
    fun readAt(offset: Long, buf: ByteArray, len: Int) {
      java.util.Arrays.fill(buf, 0, len, 0)
      var o = offset
      var i = 0
      while (i < len) {
        val n: Int
        when {
          o < SUPERBLOCK -> {
            n = minOf((SUPERBLOCK - o).toInt(), len - i)
            superblock(o, buf, i, n)
          }
          o < layout.dataStart -> {
            n = minOf((layout.dataStart - o).toInt(), len - i)
            fatBytes(o - SUPERBLOCK, buf, i, n)
          }
          else -> {
            val rel = o - layout.dataStart
            val c = rel / layout.clusterSize + 1
            val inC = rel % layout.clusterSize
            n = minOf((layout.clusterSize - inC).toInt(), len - i)
            clusterBytes(c, inC, buf, i, n)
          }
        }
        o += n
        i += n
      }
    }

    /** The first offset at or after `offset` that may hold data (a skip over zeros), or -1. */
    fun nextData(offset: Long): Long {
      if (offset < layout.dataStart) return offset
      val c = (offset - layout.dataStart) / layout.clusterSize + 1
      if (c <= usedClusters) return offset
      return -1
    }

    private fun superblock(o: Long, buf: ByteArray, at: Int, n: Int) {
      val sb = ByteArray(SUPERBLOCK.toInt()) { 0xFF.toByte() }
      ByteBuffer.wrap(sb).order(ByteOrder.LITTLE_ENDIAN).apply {
        putInt(0, MAGIC)
        putInt(4, volumeId)
        putInt(8, layout.sectorsPerCluster)
        putInt(12, 1)                     // the root directory's cluster
        putShort(16, 0)                   // then 0xFF to the end
      }
      System.arraycopy(sb, o.toInt(), buf, at, n)
    }

    /** FAT entry for cluster c: the next of its chain, the end, or free. */
    private fun fatEntry(c: Long): Long {
      if (c == 0L) return layout.media
      if (c > usedClusters) return 0
      val e = starts.floorEntry(c) ?: return 0
      val node = e.value
      val last = e.key + node.clusters - 1
      return if (c < last) c + 1 else layout.endOfChain
    }

    private fun fatBytes(rel: Long, buf: ByteArray, at: Int, n: Int) {
      val es = layout.entrySize
      for (k in 0 until n) {
        val byteOff = rel + k
        val c = byteOff / es
        if (c > layout.clusters) continue
        val v = fatEntry(c)
        buf[at + k] = (v ushr (8 * (byteOff % es).toInt())).toByte()
      }
    }

    private fun clusterBytes(c: Long, inC: Long, buf: ByteArray, at: Int, n: Int) {
      if (c > usedClusters) return
      val e = starts.floorEntry(c) ?: return
      val node = e.value
      val fileOff = (c - e.key) * layout.clusterSize + inC
      if (node.isDirectory) {
        dirBytes(node, fileOff, buf, at, n)
      } else if (fileOff < node.size) {
        val m = minOf(n.toLong(), node.size - fileOff).toInt()
        val tmp = if (at == 0) buf else ByteArray(m)
        node.source?.invoke(fileOff, tmp, m)
        if (at != 0) System.arraycopy(tmp, 0, buf, at, m)
      }
    }

    private fun dirBytes(dir: Node, off: Long, buf: ByteArray, at: Int, n: Int) {
      val total = dir.clusters * layout.clusterSize
      val image = ByteArray(minOf(total, off + n).toInt())
      java.util.Arrays.fill(image, 0xFF.toByte())       // unused entries: the end marker
      val b = ByteBuffer.wrap(image).order(ByteOrder.LITTLE_ENDIAN)
      dir.children.forEachIndexed { i, ch ->
        val o = i * ENTRY
        if (o + ENTRY > image.size) return@forEachIndexed
        val name = ch.name.toByteArray(Charsets.ISO_8859_1).take(NAME_MAX).toByteArray()
        image[o] = name.size.toByte()
        image[o + 1] = (if (ch.isDirectory) ATTR_DIR else 0).toByte()
        System.arraycopy(name, 0, image, o + 2, name.size)
        b.putInt(o + 0x2C, ch.first.toInt())
        b.putInt(o + 0x30, if (ch.isDirectory) 0 else ch.size.toInt())
        val t = toFat(ch.written)
        b.putInt(o + 0x34, t)
        b.putInt(o + 0x38, t)
        b.putInt(o + 0x3C, t)
      }
      System.arraycopy(image, off.toInt(), buf, at, n)
    }
  }

  /** FAT time (low word) and date (high word), years from 2000, to unix ms (UTC). */
  fun fromFat(v: Int): Long {
    if (v == 0 || v == -1) return 0
    val time = v and 0xFFFF
    val date = (v ushr 16) and 0xFFFF
    val c = Calendar.getInstance(TimeZone.getTimeZone("UTC"))
    c.clear()
    c.set(2000 + (date ushr 9), ((date ushr 5) and 0xF) - 1, date and 0x1F,
          time ushr 11, (time ushr 5) and 0x3F, (time and 0x1F) * 2)
    return c.timeInMillis
  }

  fun toFat(ms: Long): Int {
    val c = Calendar.getInstance(TimeZone.getTimeZone("UTC"))
    c.timeInMillis = if (ms > 0) ms else 946684800000L  // 2000-01-01
    val year = (c.get(Calendar.YEAR) - 2000).coerceIn(0, 127)
    val date = (year shl 9) or ((c.get(Calendar.MONTH) + 1) shl 5) or c.get(Calendar.DAY_OF_MONTH)
    val time = (c.get(Calendar.HOUR_OF_DAY) shl 11) or (c.get(Calendar.MINUTE) shl 5) or (c.get(Calendar.SECOND) / 2)
    return (date shl 16) or time
  }
}
