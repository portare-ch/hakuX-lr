package com.rfandango.haku_x.data

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import androidx.documentfile.provider.DocumentFile
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel

/**
 * Rewrite the game partition of a full Xbox dump as an XISO, then move the original into
 * ORIGINALS_FOLDER. Match extract-xiso's layout. The volume descriptor is at 0x10000, the
 * root is at sector 0x108, and directories precede their files and subdirectories. Directory
 * tables are binary trees ordered by uppercased name. Align entries to four bytes and keep
 * each inside one sector. Pad tables and files with 0xFF to sector boundaries, then the image
 * with zeros to 64 KiB. Copy file bytes verbatim.
 */
object XisoConverter {
  /** Where a converted dump goes, beside its XISO (the library doesn't list it). */
  const val ORIGINALS_FOLDER = "Original ISOs"

  sealed interface Result {
    /** The XISO; originalMoved is false when the dump couldn't be moved and stayed in place. */
    data class Done(val xiso: DocumentFile, val originalMoved: Boolean) : Result
    data class Failed(val message: String) : Result
    data object Cancelled : Result
  }

  private const val SECTOR = 2048
  private const val VOLUME = 0x10000L
  private const val ROOT_SECTOR = 0x108L
  private const val IMAGE_MODULUS = 0x10000L
  private const val MAGIC = "MICROSOFT*XBOX*MEDIA"
  private const val ATTR_DIR = 0x10
  private const val MASK = 0xFFFFFFFFL
  private const val COPY_CHUNK = 4 shl 20

  /** "Game (USA).iso" -> "Game (USA).xiso.iso". */
  fun outputName(fileName: String): String = fileName.substringBeforeLast('.') + ".xiso.iso"

  /**
   * Converts game (a file under the games folder). Blocking; progress gets
   * the bytes copied so far and the total, cancelled is polled between chunks.
   */
  fun convert(
    context: Context,
    folder: Uri,
    game: Game,
    progress: (Long, Long) -> Unit,
    cancelled: () -> Boolean,
  ): Result {
    val resolver = context.contentResolver
    val parent = parentOf(context, folder, game.relativePath)
      ?: return Result.Failed("Can't open the game's folder")
    val fileName = game.relativePath.substringAfterLast('/')
    val name = outputName(fileName)
    if (parent.findFile(name) != null) return Result.Failed("$name already exists")

    var out: DocumentFile? = null
    try {
      val input = resolver.openFileDescriptor(game.uri, "r") ?: throw IOException("Can't read ${game.relativePath}")
      input.use { pfd ->
        FileInputStream(pfd.fileDescriptor).use { fis ->
          val src = fis.channel
          val partition = XboxDisc.partitionOf(src) ?: throw IOException("Not an Xbox disc image")
          val vd = readAt(src, partition + VOLUME + MAGIC.length, 16)
          val root = Entry(ByteArray(0), ATTR_DIR, 0, 0, null)
          root.children = readDir(src, partition, vd.getInt(0).toLong() and MASK, vd.getInt(4).toLong() and MASK, 0)
          val fileTime = ByteArray(8).also { vd.position(8); vd.get(it) }
          layout(root)
          val total = totalBytes(root)

          val created = parent.createFile("application/octet-stream", name) ?: throw IOException("Can't create $name")
          out = created
          val output = resolver.openFileDescriptor(created.uri, "w") ?: throw IOException("Can't write $name")
          output.use { opfd ->
            FileOutputStream(opfd.fileDescriptor).use { fos ->
              val w = Writer(fos.channel)
              writeHeader(w, root, fileTime)
              var copied = 0L
              emit(w, src, partition, root) { n ->
                copied += n
                progress(copied, total)
                if (cancelled()) throw CancelledException()
              }
              w.zeros((IMAGE_MODULUS - w.position % IMAGE_MODULUS) % IMAGE_MODULUS)
              w.flush()
            }
          }
        }
      }
    } catch (_: CancelledException) {
      out?.delete()
      return Result.Cancelled
    } catch (e: Exception) {
      out?.delete()
      return Result.Failed(e.message ?: e.toString())
    }

    // The dump goes into the originals folder beside the XISO.
    val moved = try {
      val originals = parent.findFile(ORIGINALS_FOLDER)?.takeIf { it.isDirectory }
        ?: parent.createDirectory(ORIGINALS_FOLDER)
      originals != null && DocumentsContract.moveDocument(resolver, game.uri, parent.uri, originals.uri) != null
    } catch (_: Exception) {
      false
    }
    return Result.Done(out!!, moved)
  }

  private class CancelledException : Exception()

  /** A directory entry: the source's, then where it goes in the XISO. */
  private class Entry(
    val name: ByteArray,
    val attributes: Int,
    val sector: Long,
    val size: Long,
    var children: List<Entry>?,
  ) {
    val isDir get() = attributes and ATTR_DIR != 0
    var outSector = 0L
    /** Directories: the table's bytes (an empty directory has a padding sector). */
    var tableSize = 0
    /** Directories: the children in table order, a binary tree's prefix order. */
    var order: List<Entry> = emptyList()
    var offset = 0
    var left: Entry? = null
    var right: Entry? = null
  }

  private fun parentOf(context: Context, folder: Uri, relativePath: String): DocumentFile? {
    var dir = DocumentFile.fromTreeUri(context, folder) ?: return null
    for (segment in relativePath.split('/').dropLast(1)) {
      dir = dir.findFile(segment)?.takeIf { it.isDirectory } ?: return null
    }
    return dir
  }

  private fun readAt(ch: FileChannel, pos: Long, n: Int): ByteBuffer {
    val buf = ByteBuffer.allocate(n).order(ByteOrder.LITTLE_ENDIAN)
    var at = pos
    while (buf.hasRemaining()) {
      val got = ch.read(buf, at)
      if (got <= 0) throw IOException("The image ends early")
      at += got
    }
    buf.flip()
    return buf
  }

  /** A directory table's entries, walking its tree (an all-0xFF table is empty). */
  private fun readDir(ch: FileChannel, partition: Long, sector: Long, size: Long, depth: Int): List<Entry> {
    if (size == 0L) return emptyList()
    if (depth > 64 || size > 64L shl 20) throw IOException("Directory tree too deep or too large")
    val t = readAt(ch, partition + sector * SECTOR, size.toInt())
    val out = ArrayList<Entry>()
    val seen = HashSet<Int>()
    val pending = ArrayDeque<Int>().apply { add(0) }
    while (pending.isNotEmpty()) {
      val off = pending.removeLast()
      if (off < 0 || off + 14 > t.capacity() || !seen.add(off)) continue
      val left = t.getShort(off).toInt() and 0xFFFF
      if (left == 0xFFFF) continue // padding
      val right = t.getShort(off + 2).toInt() and 0xFFFF
      val nameLen = t.get(off + 13).toInt() and 0xFF
      if (off + 14 + nameLen > t.capacity()) continue
      val entry = Entry(
        name = ByteArray(nameLen) { t.get(off + 14 + it) },
        attributes = t.get(off + 12).toInt() and 0xFF,
        sector = t.getInt(off + 4).toLong() and MASK,
        size = t.getInt(off + 8).toLong() and MASK,
        children = null,
      )
      if (entry.isDir) entry.children = readDir(ch, partition, entry.sector, entry.size, depth + 1)
      out += entry
      if (right != 0) pending.add(right * 4)
      if (left != 0) pending.add(left * 4)
    }
    return out
  }

  /** extract-xiso's name order: ASCII upper-cased, bytes compared as C chars. */
  private val NAME_ORDER = Comparator<Entry> { a, b ->
    val n = minOf(a.name.size, b.name.size)
    for (i in 0 until n) {
      var x = a.name[i].toInt()
      var y = b.name[i].toInt()
      if (x in 'a'.code..'z'.code) x -= 32
      if (y in 'a'.code..'z'.code) y -= 32
      if (x != y) return@Comparator x.compareTo(y)
    }
    a.name.size.compareTo(b.name.size)
  }

  private fun nSectors(bytes: Long): Long = (bytes + SECTOR - 1) / SECTOR

  /** Tables and sectors for the whole tree, in the order the image is written. */
  private fun layout(root: Entry) {
    tables(root)
    var next = ROOT_SECTOR
    fun place(dir: Entry) {
      dir.outSector = next
      next += nSectors(dir.tableSize.toLong())
      for (e in dir.order) {
        if (!e.isDir) {
          e.outSector = next
          next += nSectors(e.size)
        }
      }
      for (e in dir.order) if (e.isDir) place(e)
    }
    place(root)
  }

  /** Each directory's table: a balanced tree over its sorted names, entries placed in prefix order. */
  private fun tables(dir: Entry) {
    val sorted = dir.children.orEmpty().sortedWith(NAME_ORDER)
    val order = ArrayList<Entry>(sorted.size)
    fun build(lo: Int, hi: Int): Entry? {
      if (lo > hi) return null
      val mid = (lo + hi) / 2
      val node = sorted[mid]
      order += node
      node.left = build(lo, mid - 1)
      node.right = build(mid + 1, hi)
      return node
    }
    build(0, sorted.size - 1)
    dir.order = order
    var size = 0
    for (e in order) {
      val len = (14 + e.name.size + 3) and 3.inv()
      if (size / SECTOR != (size + len - 1) / SECTOR) size = (size + SECTOR - 1) / SECTOR * SECTOR
      e.offset = size
      size += len
    }
    dir.tableSize = if (order.isEmpty()) SECTOR else size
    for (e in order) if (e.isDir) tables(e)
  }

  private fun totalBytes(dir: Entry): Long =
    dir.order.sumOf { if (it.isDir) totalBytes(it) else it.size }

  /** Sequential output with a known position. */
  private class Writer(private val ch: FileChannel) {
    var position = 0L
      private set
    private val buf = ByteBuffer.allocateDirect(COPY_CHUNK)

    fun put(b: ByteBuffer) {
      while (b.hasRemaining()) position += ch.write(b)
    }

    fun put(bytes: ByteArray) = put(ByteBuffer.wrap(bytes))

    fun fill(n: Long, value: Byte) {
      var left = n
      while (left > 0) {
        val k = minOf(left, buf.capacity().toLong()).toInt()
        buf.clear()
        for (i in 0 until k) buf.put(value)
        buf.flip()
        put(buf)
        left -= k
      }
    }

    fun zeros(n: Long) = fill(n, 0)

    fun seekTo(pos: Long) {
      check(pos >= position) { "layout out of order at $position, wanted $pos" }
      zeros(pos - position)
    }

    /** n bytes of src from pos; each chunk reported to copied. */
    fun copy(src: FileChannel, pos: Long, n: Long, copied: (Long) -> Unit) {
      var at = pos
      var left = n
      while (left > 0) {
        buf.clear()
        buf.limit(minOf(left, buf.capacity().toLong()).toInt())
        while (buf.hasRemaining()) {
          val got = src.read(buf, at + buf.position())
          if (got <= 0) throw IOException("The image ends early")
        }
        buf.flip()
        val k = buf.remaining()
        put(buf)
        at += k
        left -= k
        copied(k.toLong())
      }
    }

    fun flush() = ch.force(false)
  }

  /** The volume descriptor at 0x10000: magic, root directory sector and size, filetime, magic. */
  private fun writeHeader(w: Writer, root: Entry, fileTime: ByteArray) {
    w.zeros(VOLUME)
    val vd = ByteBuffer.allocate(SECTOR).order(ByteOrder.LITTLE_ENDIAN)
    vd.put(MAGIC.toByteArray(Charsets.US_ASCII))
    vd.putInt(root.outSector.toInt())
    vd.putInt(root.tableSize)
    vd.put(fileTime)
    vd.position(SECTOR - MAGIC.length)
    vd.put(MAGIC.toByteArray(Charsets.US_ASCII))
    vd.flip()
    w.put(vd)
  }

  /** A directory's table, its files, then its subdirectories. */
  private fun emit(w: Writer, src: FileChannel, partition: Long, dir: Entry, copied: (Long) -> Unit) {
    w.seekTo(dir.outSector * SECTOR)
    val table = ByteBuffer.allocate((nSectors(dir.tableSize.toLong()) * SECTOR).toInt()).order(ByteOrder.LITTLE_ENDIAN)
    for (i in 0 until table.capacity()) table.put(i, 0xFF.toByte())
    for (e in dir.order) {
      table.position(e.offset)
      table.putShort(((e.left?.offset ?: 0) / 4).toShort())
      table.putShort(((e.right?.offset ?: 0) / 4).toShort())
      table.putInt(e.outSector.toInt())
      table.putInt(if (e.isDir) (nSectors(e.tableSize.toLong()) * SECTOR).toInt() else e.size.toInt())
      table.put(e.attributes.toByte())
      table.put(e.name.size.toByte())
      table.put(e.name)
    }
    table.position(0)
    w.put(table)
    for (e in dir.order) {
      if (e.isDir) continue
      w.seekTo(e.outSector * SECTOR)
      w.copy(src, partition + e.sector * SECTOR, e.size, copied)
      w.fill((SECTOR - e.size % SECTOR) % SECTOR, 0xFF.toByte())
    }
    for (e in dir.order) if (e.isDir) emit(w, src, partition, e, copied)
  }
}
