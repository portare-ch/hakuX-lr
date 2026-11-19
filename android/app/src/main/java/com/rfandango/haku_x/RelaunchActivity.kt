package com.rfandango.haku_x

import android.app.Activity
import android.app.ActivityManager
import android.app.ActivityOptions
import android.content.Context
import android.content.Intent
import android.graphics.BitmapFactory
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.widget.ImageView
import com.rfandango.haku_x.ui.theme.AppTheme
import java.io.File

/**
 * Wait in the main process for the previous game process to exit before starting a fresh
 * core. This handles XLaunchNewImage, loading a save state and restarting from the beginning
 * after a console identity change. Show the last game frame and suppress the activity
 * transition so the reboot looks like in-game loading.
 */
class RelaunchActivity : Activity() {
  private val handler = Handler(Looper.getMainLooper())
  private var loading: LoadingDialog? = null

  override fun onCreate(savedInstanceState: Bundle?) {
    /* the savestate's loading dialog light or dark, as NativeCoreActivity's after it */
    if (AppTheme.isDark(this)) {
      setTheme(R.style.Theme_HakuX_Game_Dark)
    }
    super.onCreate(savedInstanceState)
    setContentView(ImageView(this).apply {
      scaleType = ImageView.ScaleType.FIT_XY
      heldFrame(this@RelaunchActivity).takeIf { it.exists() }?.let { BitmapFactory.decodeFile(it.path) }
        ?.let(::setImageBitmap)
    })
    /* a savestate's load: its dialog, until NativeCoreActivity shows its own */
    loading = intent.getStringExtra(NativeCoreActivity.EXTRA_LOADSTATE)?.let { LoadingDialog(this, it).also { d -> d.show() } }
    hideSystemBars()      /* after the content: the window's decor exists */
    val am = getSystemService(ACTIVITY_SERVICE) as ActivityManager
    val coreProcess = "$packageName:native"
    val start = SystemClock.uptimeMillis()
    val poll = object : Runnable {
      override fun run() {
        val alive = am.runningAppProcesses?.any { it.processName == coreProcess } == true
        if (alive && SystemClock.uptimeMillis() - start < WAIT_MS) {
          handler.postDelayed(this, 20)
          return
        }
        Log.i(TAG, "relaunch: core process gone after ${SystemClock.uptimeMillis() - start} ms")
        startCore()
      }
    }
    handler.post(poll)
  }

  private fun startCore() {
    val core = Intent(this, NativeCoreActivity::class.java)
      .putExtra(NativeCoreActivity.EXTRA_DVD_URI, intent.getStringExtra(NativeCoreActivity.EXTRA_DVD_URI))
      .apply {
        /* a savestate's load, the title's own reboot, or the game from its start */
        val state = intent.getStringExtra(NativeCoreActivity.EXTRA_LOADSTATE)
        if (state != null) putExtra(NativeCoreActivity.EXTRA_LOADSTATE, state)
        else if (!intent.getBooleanExtra(NativeCoreActivity.EXTRA_RESTART, false)) {
          putExtra(NativeCoreActivity.EXTRA_RELAUNCH, true)
        }
      }
      .putExtra(
        LauncherActivity.EXTRA_LAUNCHED_EXTERNALLY,
        intent.getBooleanExtra(LauncherActivity.EXTRA_LAUNCHED_EXTERNALLY, false),
      )
      .addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION)
    startActivity(core, noTransition(this).toBundle())
    finish()
    noCloseTransition(this)
  }

  private fun hideSystemBars() {
    if (Build.VERSION.SDK_INT >= 30) {
      window.insetsController?.let {
        it.hide(WindowInsets.Type.systemBars())
        it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
      }
    }
  }

  override fun onDestroy() {
    handler.removeCallbacksAndMessages(null)
    loading?.dismiss()
    super.onDestroy()
  }

  companion object {
    private const val TAG = "hakuX-library"
    private const val WAIT_MS = 3000L

    /** The frame on screen when the title rebooted, held across the core's restart. */
    fun heldFrame(context: Context) = File(context.cacheDir, "relaunch_frame.jpg")

    /** An activity start with no window animation and, on Android 13+, a splash without the icon. */
    fun noTransition(context: Context): ActivityOptions {
      val o = ActivityOptions.makeCustomAnimation(context, 0, 0)
      if (Build.VERSION.SDK_INT >= 33) {
        o.setSplashScreenStyle(android.window.SplashScreen.SPLASH_SCREEN_STYLE_SOLID_COLOR)
      }
      return o
    }

    /** No animation as the finishing activity goes. */
    @Suppress("DEPRECATION")
    fun noCloseTransition(activity: Activity) {
      if (Build.VERSION.SDK_INT >= 34) {
        activity.overrideActivityTransition(Activity.OVERRIDE_TRANSITION_CLOSE, 0, 0)
      } else {
        activity.overridePendingTransition(0, 0)
      }
    }
  }
}
