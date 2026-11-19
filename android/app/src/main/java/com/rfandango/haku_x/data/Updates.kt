package com.rfandango.haku_x.data

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.content.pm.PackageManager
import android.os.Build
import android.util.Log
import android.widget.Toast
import androidx.core.content.IntentCompat
import com.rfandango.haku_x.BuildConfig
import com.rfandango.haku_x.R
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Check rfandango/hakuX releases at startup and on demand in settings. If a newer APK is
 * available, show its notes, download it to the cache, verify its package and signer, then
 * let the Android installer prompt the user.
 */
object Updates {
  private const val TAG = "hakuX-update"
  private const val REPO = "rfandango/hakuX"
  /** The release builds' application id (a debug build's has ".debug"): what the APK must be. */
  private const val RELEASE_PACKAGE = "com.rfandango.haku_x"
  private const val TIMEOUT_MS = 15_000

  class Release(
    /** "1.1.0" (the tag without its "v"). */
    val version: String,
    val title: String,
    /** The release notes, Markdown. */
    val notes: String,
    val apkUrl: String,
    val apkSize: Long,
  )

  private val _available = MutableStateFlow<Release?>(null)

  /** The newer release a check found (the library's update button), or null. */
  val available: StateFlow<Release?> = _available

  private val startChecked = AtomicBoolean(false)

  /**
   * The version this app is: its versionName. Debug builds take the one in files/debug_update_version
   * instead, when there is one (to try the update path against an older version).
   */
  fun installedVersion(context: Context): String {
    if (BuildConfig.DEBUG) {
      runCatching { File(context.filesDir, "debug_update_version").readText().trim() }.getOrNull()
        ?.takeIf { it.isNotEmpty() }?.let { return it }
    }
    return BuildConfig.VERSION_NAME
  }

  /**
   * The latest release, when newer than this app (null: none newer, or no release with an APK).
   * Network and format failures throw.
   */
  suspend fun check(context: Context): Release? = withContext(Dispatchers.IO) {
    val conn = open("https://api.github.com/repos/$REPO/releases/latest")
    conn.setRequestProperty("Accept", "application/vnd.github+json")
    try {
      when (conn.responseCode) {
        HttpURLConnection.HTTP_OK -> Unit
        HttpURLConnection.HTTP_NOT_FOUND -> return@withContext null    // no release yet
        else -> throw IOException("GitHub answered ${conn.responseCode}")
      }
      val json = JSONObject(conn.inputStream.bufferedReader().use { it.readText() })
      val version = json.getString("tag_name").trim().removePrefix("v").removePrefix("V")
      val assets = json.optJSONArray("assets")
      val apk = (0 until (assets?.length() ?: 0)).map { assets!!.getJSONObject(it) }
        .firstOrNull { it.optString("name").endsWith(".apk", ignoreCase = true) }
      val newer = compareVersions(version, installedVersion(context)) > 0
      Log.i(TAG, "latest release $version (this app ${installedVersion(context)}): " +
        if (apk == null) "no APK" else if (newer) "newer" else "not newer")
      if (apk == null || !newer) return@withContext null
      Release(
        version = version,
        title = json.optString("name").ifBlank { "hakuX $version" },
        notes = json.optString("body"),
        apkUrl = apk.getString("browser_download_url"),
        apkSize = apk.optLong("size"),
      )
    } finally {
      conn.disconnect()
    }
  }.also { _available.value = it }

  /**
   * The check as the app's screens start: once a process, quietly (a failure is only logged).
   * Leftover downloads go first. The release found, or null.
   */
  suspend fun checkAtStart(context: Context): Release? {
    if (!startChecked.compareAndSet(false, true)) return null
    withContext(Dispatchers.IO) { File(context.cacheDir, "updates").deleteRecursively() }
    return runCatching { check(context) }
      .onFailure { Log.w(TAG, "check at start: ${it.message}") }
      .getOrNull()
  }

  /** a < b: negative; numeric parts compared in turn ("1.10" after "1.9"), a missing one as 0. */
  fun compareVersions(a: String, b: String): Int {
    fun parts(v: String) = v.split('.', '-', '+').map { p -> p.takeWhile(Char::isDigit).toIntOrNull() ?: 0 }
    val x = parts(a)
    val y = parts(b)
    for (i in 0 until maxOf(x.size, y.size)) {
      val c = x.getOrElse(i) { 0 }.compareTo(y.getOrElse(i) { 0 })
      if (c != 0) return c
    }
    return 0
  }

  /**
   * The release's APK, downloaded into the cache (progress: bytes so far, the total or 0), and
   * checked: this app's package, signed as this app is (a release build; a debug build's key is
   * not the release's). Cancelled with its coroutine.
   */
  suspend fun download(context: Context, release: Release, progress: (Long, Long) -> Unit): File =
    withContext(Dispatchers.IO) {
      val dir = File(context.cacheDir, "updates").apply { deleteRecursively(); mkdirs() }
      val apk = File(dir, "hakuX-${release.version}.apk")
      val part = File(dir, apk.name + ".part")
      val conn = open(release.apkUrl)
      try {
        if (conn.responseCode != HttpURLConnection.HTTP_OK) throw IOException("the download answered ${conn.responseCode}")
        val total = conn.contentLengthLong.takeIf { it > 0 } ?: release.apkSize
        var done = 0L
        conn.inputStream.use { input ->
          part.outputStream().use { out ->
            val buf = ByteArray(1 shl 16)
            while (true) {
              currentCoroutineContext().ensureActive()
              val n = input.read(buf)
              if (n < 0) break
              out.write(buf, 0, n)
              done += n
              progress(done, total)
            }
          }
        }
        if (total > 0 && done != total) throw IOException("the download stopped at $done of $total bytes")
      } finally {
        conn.disconnect()
      }
      if (!part.renameTo(apk)) throw IOException("can't keep the download")
      verify(context, apk)
      apk
    }

  /** The APK is hakuX, and (a release build) signed with this app's key. */
  private fun verify(context: Context, apk: File) {
    val pm = context.packageManager
    val flags = if (Build.VERSION.SDK_INT >= 28) PackageManager.GET_SIGNING_CERTIFICATES else 0
    val info = pm.getPackageArchiveInfo(apk.path, flags) ?: throw IOException("the download isn't an app")
    if (info.packageName != RELEASE_PACKAGE) throw IOException("the download is ${info.packageName}, not hakuX")
    if (context.packageName == RELEASE_PACKAGE) {
      val ours = pm.getPackageInfo(context.packageName, PackageManager.GET_SIGNING_CERTIFICATES)
        .signingInfo?.apkContentsSigners.orEmpty().map { it.toCharsString() }.toSet()
      val theirs = info.signingInfo?.apkContentsSigners.orEmpty().map { it.toCharsString() }.toSet()
      if (ours.isEmpty() || ours != theirs) throw IOException("the download isn't signed as this app")
    }
  }

  /**
   * The APK to the system installer (a PackageInstaller session): it asks the user, then replaces
   * this app (UpdateInstallReceiver takes its answers).
   */
  suspend fun install(context: Context, apk: File) = withContext(Dispatchers.IO) {
    val installer = context.packageManager.packageInstaller
    val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
    params.setAppPackageName(RELEASE_PACKAGE)
    params.setSize(apk.length())
    val id = installer.createSession(params)
    installer.openSession(id).use { session ->
      session.openWrite("hakuX.apk", 0, apk.length()).use { out ->
        apk.inputStream().use { it.copyTo(out, 1 shl 16) }
        session.fsync(out)
      }
      val flags = PendingIntent.FLAG_UPDATE_CURRENT or
        (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
      val status = PendingIntent.getBroadcast(
        context, id, Intent(context, UpdateInstallReceiver::class.java).setPackage(context.packageName), flags,
      )
      session.commit(status.intentSender)
    }
  }

  private fun open(url: String): HttpURLConnection =
    (URL(url).openConnection() as HttpURLConnection).apply {
      connectTimeout = TIMEOUT_MS
      readTimeout = TIMEOUT_MS
      instanceFollowRedirects = true      // a release asset redirects to GitHub's file host
      setRequestProperty("User-Agent", "hakuX/${BuildConfig.VERSION_NAME} (Android)")
    }
}

/**
 * The system installer's answers about an update (Updates.install): its confirmation to show (the
 * user's), or why it failed. On success this app is replaced, its process gone.
 */
class UpdateInstallReceiver : BroadcastReceiver() {
  override fun onReceive(context: Context, intent: Intent) {
    val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)
    when (status) {
      PackageInstaller.STATUS_PENDING_USER_ACTION -> {
        IntentCompat.getParcelableExtra(intent, Intent.EXTRA_INTENT, Intent::class.java)?.let {
          context.startActivity(it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        }
      }
      PackageInstaller.STATUS_SUCCESS -> Log.i("hakuX-update", "installed")
      PackageInstaller.STATUS_FAILURE_ABORTED -> Log.i("hakuX-update", "install cancelled")
      else -> {
        val why = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: "status $status"
        Log.w("hakuX-update", "install failed: $why")
        Toast.makeText(context, context.getString(R.string.update_install_failed, why), Toast.LENGTH_LONG).show()
      }
    }
  }
}
