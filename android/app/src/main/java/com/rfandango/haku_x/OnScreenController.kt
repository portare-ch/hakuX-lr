package com.rfandango.haku_x

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.PointF
import android.graphics.RectF
import android.graphics.RenderNode
import android.os.Build
import android.os.SystemClock
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import androidx.annotation.RequiresApi
import com.rfandango.haku_x.overlay.ControlId
import com.rfandango.haku_x.overlay.ControlKind
import com.rfandango.haku_x.overlay.ControlPainter
import com.rfandango.haku_x.overlay.ControlSpec
import com.rfandango.haku_x.overlay.OverlayLayout
import com.rfandango.haku_x.overlay.OverlayStore
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.roundToInt

/**
 * The on-screen controller, drawn like the original Xbox controller, each
 * control where and as large as the user put it (OverlayStore).
 *
 * Playing, touches drive the controls (ControllerListener). Editing, a
 * control is dragged to move it and resized by its corner handle or a pinch;
 * the editor bar (OverlayEditorBar) sizes, hides and resets, and saves.
 */
class OnScreenController @JvmOverloads constructor(
  context: Context,
  attrs: AttributeSet? = null,
  defStyleAttr: Int = 0,
) : View(context, attrs, defStyleAttr) {

  enum class Button {
    A, B, X, Y,
    DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
    LEFT_TRIGGER, RIGHT_TRIGGER,
    START, BACK,
    LEFT_STICK_BUTTON, RIGHT_STICK_BUTTON,
    BLACK, WHITE,
  }

  enum class Stick { LEFT, RIGHT }

  interface ControllerListener {
    fun onButtonPressed(button: Button)
    fun onButtonReleased(button: Button)
    fun onStickMoved(stick: Stick, x: Float, y: Float)
    fun onStickPressed(stick: Stick)
    fun onStickReleased(stick: Stick)
  }

  /** The jewel was tapped, or the swipe up from the lower screen. */
  var onMenuButtonTapped: (() -> Unit)? = null
  /** Editing: the selected control changed (null: none). */
  var onSelectionChanged: ((ControlId?) -> Unit)? = null
  /** Editing: a size, visibility or the opacity changed. */
  var onLayoutChanged: (() -> Unit)? = null
  /**
   * Playing: whether the controls may redraw now, wanted since the given
   * uptime (ms); else they look as they did a while longer, the input already
   * sent (OverlayPacer). Null: at once.
   */
  var drawGate: ((Long) -> Boolean)? = null

  private var listener: ControllerListener? = null
  private val painter = ControlPainter()
  private var layout: OverlayLayout? = null
  private var defaults: OverlayLayout? = null
  private val density = resources.displayMetrics.density

  // ---------------------------------------------------------------- playing
  private val held = HashMap<ControlId, Int>()          /* button controls: the pointer holding each */
  private val stickPointer = intArrayOf(-1, -1)
  private val stickPos = arrayOf(PointF(), PointF())
  private var dpadPointer = -1
  private var dpadBits = 0
  private var menuPointer = -1
  private var menuPressed = false

  /** Only the jewel shows (a gamepad is attached, or the user hid the controls). */
  var minimal = false
    set(value) {
      if (field != value) {
        field = value
        resetAllInputs()
      }
    }

  /** Minimal, not even the jewel (a gamepad is attached): the swipe up and back open the menu. */
  var jewelHidden = false
    set(value) {
      if (field != value) {
        field = value
        resetAllInputs()
      }
    }

  /** Not drawn and not touchable while playing. */
  private fun off(s: ControlSpec) = !s.visible || (minimal && (s.id != ControlId.MENU || jewelHidden))

  private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop.toFloat()
  private val swipeUp = SwipeUpGestureRecognizer(
    minDistancePx = { max(height * 0.14f, touchSlop * 8f) },
    touchSlopPx = { touchSlop },
    canStartAt = { x, y -> y >= height * 0.35f && hit(x, y) == null },
    onTriggered = {
      releaseAll()
      onMenuButtonTapped?.invoke()
      invalidate()
    },
  )

  // ---------------------------------------------------------------- editing
  /** Editing the layout instead of playing: touches move and size the controls. */
  var editing = false
    set(value) {
      if (field == value) return
      field = value
      resetAllInputs()
      select(null)
      invalidate()
    }
  private var selected: ControlId? = null
  private var dragPointer = -1
  private var dragMode = Drag.NONE
  private val dragOffset = PointF()
  private var pinchStart = 0f
  private var pinchStartRadius = 0f

  private enum class Drag { NONE, MOVE, RESIZE, PINCH }

  private val editPaint = Paint(Paint.ANTI_ALIAS_FLAG)
  private val dash = DashPathEffect(floatArrayOf(10f * density, 6f * density), 0f)
  private val box = RectF()

  init {
    setBackgroundColor(Color.TRANSPARENT)
  }

  fun setControllerListener(listener: ControllerListener) {
    this.listener = listener
  }

  override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
    super.onSizeChanged(w, h, oldw, oldh)
    if (w == 0 || h == 0) return
    val current = layout
    if (current == null || oldw == 0 || oldh == 0) {
      layout = OverlayStore.load(context, w.toFloat(), h.toFloat())
    } else {
      // Keep (unsaved) edits through a size change: everything scales with the view.
      val sx = w.toFloat() / oldw
      val sy = h.toFloat() / oldh
      for (s in current.controls.values) {
        s.cx *= sx
        s.cy *= sy
        s.radius *= sy
      }
    }
    defaults = OverlayStore.defaults(w.toFloat(), h.toFloat())
  }

  // =========================================================== drawing

  override fun onDraw(canvas: Canvas) {
    super.onDraw(canvas)
    val l = layout ?: return
    if (editing) {
      drawEditBackdrop(canvas)
    }
    val cached = !editing && Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && canvas.isHardwareAccelerated
    for (s in l.controls.values) {
      if (!editing && off(s)) {
        continue
      }
      // Each control at its own opacity; editing, a hidden one stays faint
      // where it is, so it can be selected and shown again.
      val opacity = if (s.visible) s.opacity else 0.25f
      if (cached) {
        drawCached(canvas, s, opacity)
        continue
      }
      val b = s.radius * ControlPainter.reach(s.id.kind) * 1.3f
      canvas.saveLayerAlpha(s.cx - b, s.cy - b, s.cx + b, s.cy + b, (opacity * 255).roundToInt())
      drawControl(canvas, s)
      canvas.restore()
    }
    if (editing) {
      val sel = selected
      if (sel != null) {
        drawSelection(canvas, l[sel])
      } else {
        editPaint.style = Paint.Style.FILL
        editPaint.textSize = 15f * density
        editPaint.textAlign = Paint.Align.CENTER
        editPaint.color = Color.argb(220, 255, 255, 255)
        editPaint.setShadowLayer(3f * density, 0f, 1f * density, Color.BLACK)
        canvas.drawText(hint, width / 2f, height * 0.42f, editPaint)
        editPaint.clearShadowLayer()
      }
    }
  }

  private val hint by lazy { context.getString(R.string.overlay_editor_hint) }

  /*
   * During play, record each control in its own display list and GPU layer. Re-record only
   * when appearance or position changes, and apply opacity during compositing. Rebuilding
   * gradients and layers for each touch event took about 80% of one core on a Galaxy Tab S9
   * while a stick was held.
   */
  private data class Look(
    val cx: Float, val cy: Float, val r: Float,
    val pressed: Boolean, val kx: Float, val ky: Float, val dpad: Int,
  )

  private val nodes = HashMap<ControlId, RenderNode>()
  private val looks = HashMap<ControlId, Look>()

  private fun look(s: ControlSpec): Look = when (s.id.kind) {
    ControlKind.STICK -> {
      val i = stickIndex(s.id)
      Look(s.cx, s.cy, s.radius, stickPointer[i] != -1, stickPos[i].x, stickPos[i].y, 0)
    }
    ControlKind.DPAD -> Look(s.cx, s.cy, s.radius, dpadBits != 0, 0f, 0f, dpadBits)
    ControlKind.MENU -> Look(s.cx, s.cy, s.radius, menuPressed, 0f, 0f, 0)
    else -> Look(s.cx, s.cy, s.radius, s.id in held, 0f, 0f, 0)
  }

  @RequiresApi(Build.VERSION_CODES.Q)
  private fun drawCached(canvas: Canvas, s: ControlSpec, opacity: Float) {
    val node = nodes.getOrPut(s.id) { RenderNode(s.id.name).apply { setUseCompositingLayer(true, null) } }
    val look = look(s)
    if (looks[s.id] != look || !node.hasDisplayList()) {
      looks[s.id] = look
      val b = s.radius * ControlPainter.reach(s.id.kind) * 1.3f
      val left = floor(s.cx - b).toInt()
      val top = floor(s.cy - b).toInt()
      node.setPosition(left, top, ceil(s.cx + b).toInt(), ceil(s.cy + b).toInt())
      val c = node.beginRecording()
      c.translate(-left.toFloat(), -top.toFloat())
      drawControl(c, s)
      node.endRecording()
    }
    node.setAlpha(opacity)
    canvas.drawRenderNode(node)
  }

  /* What the controls show while playing: a touch that changes none of it draws nothing. */
  private fun shown(): Int {
    var h = held.keys.hashCode()
    h = 31 * h + dpadBits
    h = 31 * h + if (menuPressed) 1 else 0
    for (i in 0..1) {
      h = 31 * h + if (stickPointer[i] != -1) 1 else 0
      h = 31 * h + stickPos[i].x.toRawBits()
      h = 31 * h + stickPos[i].y.toRawBits()
    }
    return h
  }

  private fun drawControl(canvas: Canvas, s: ControlSpec) {
    when (s.id.kind) {
      ControlKind.STICK -> {
        val i = stickIndex(s.id)
        painter.draw(canvas, s, stickPointer[i] != -1, stickPos[i].x, stickPos[i].y)
      }
      ControlKind.DPAD -> painter.draw(canvas, s, dpadBits != 0, dpad = dpadBits)
      ControlKind.MENU -> painter.draw(canvas, s, menuPressed)
      else -> painter.draw(canvas, s, s.id in held)
    }
  }

  private fun drawEditBackdrop(canvas: Canvas) {
    canvas.drawColor(Color.argb(110, 0, 0, 0))
    editPaint.style = Paint.Style.STROKE
    editPaint.pathEffect = null
    editPaint.strokeWidth = 1f
    editPaint.color = Color.argb(30, 255, 255, 255)
    val step = height / 10f
    var x = width / 2f % step
    while (x < width) {
      canvas.drawLine(x, 0f, x, height.toFloat(), editPaint)
      x += step
    }
    var y = step
    while (y < height) {
      canvas.drawLine(0f, y, width.toFloat(), y, editPaint)
      y += step
    }
  }

  private fun drawSelection(canvas: Canvas, s: ControlSpec) {
    val b = s.radius * ControlPainter.reach(s.id.kind)
    box.set(s.cx - b, s.cy - b, s.cx + b, s.cy + b)
    editPaint.style = Paint.Style.STROKE
    editPaint.strokeWidth = 2f * density
    editPaint.pathEffect = dash
    editPaint.color = ACCENT
    canvas.drawRoundRect(box, 8f * density, 8f * density, editPaint)
    editPaint.pathEffect = null

    // The resize handle on the bottom-right corner, with a diagonal double arrow.
    val hr = handleRadius()
    editPaint.style = Paint.Style.FILL
    editPaint.color = ACCENT
    canvas.drawCircle(box.right, box.bottom, hr, editPaint)
    editPaint.style = Paint.Style.STROKE
    editPaint.strokeWidth = 2f * density
    editPaint.color = Color.WHITE
    canvas.drawCircle(box.right, box.bottom, hr, editPaint)
    val a = hr * 0.45f
    val hx = box.right
    val hy = box.bottom
    canvas.drawLine(hx - a, hy - a, hx + a, hy + a, editPaint)
    canvas.drawLine(hx + a, hy + a, hx + a * 0.1f, hy + a, editPaint)
    canvas.drawLine(hx + a, hy + a, hx + a, hy + a * 0.1f, editPaint)
    canvas.drawLine(hx - a, hy - a, hx - a * 0.1f, hy - a, editPaint)
    canvas.drawLine(hx - a, hy - a, hx - a, hy - a * 0.1f, editPaint)

    editPaint.style = Paint.Style.FILL
    editPaint.textSize = 14f * density
    editPaint.textAlign = Paint.Align.CENTER
    editPaint.color = Color.WHITE
    editPaint.setShadowLayer(3f * density, 0f, 1f * density, Color.BLACK)
    val title = if (s.visible) s.id.title else "${s.id.title} (hidden)"
    val ty = if (box.top - 8f * density > 20f * density) box.top - 8f * density else box.bottom + 22f * density
    canvas.drawText(title, s.cx, ty, editPaint)
    editPaint.clearShadowLayer()
  }

  private fun handleRadius() = max(14f * density, height * 0.03f)

  // =========================================================== touches

  override fun onTouchEvent(event: MotionEvent): Boolean {
    if (layout == null) return false
    if (editing) {
      editTouch(event)
      invalidate()
    } else {
      val before = shown()
      playTouch(event)
      if (shown() != before) {
        redrawWanted()
      }
    }
    return true
  }

  /* Playing: a look changed, drawn once the gate lets it (drawGate), tried
   * again each display frame till then. */
  private var wantedSince = 0L

  private fun redrawWanted() {
    if (wantedSince == 0L) {
      wantedSince = SystemClock.uptimeMillis()
    }
    tryRedraw()
  }

  private val retryRedraw = Runnable { tryRedraw() }

  private fun tryRedraw() {
    if (wantedSince == 0L) {
      return
    }
    removeCallbacks(retryRedraw)
    if (drawGate?.invoke(wantedSince) == false) {
      postOnAnimation(retryRedraw)
      return
    }
    wantedSince = 0L
    invalidate()
  }

  // ---------------------------------------------------------------- playing

  private fun playTouch(event: MotionEvent) {
    if (swipeUp.onTouchEvent(event)) {
      return
    }
    val index = event.actionIndex
    val pointer = event.getPointerId(index)
    when (event.actionMasked) {
      MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> down(event.getX(index), event.getY(index), pointer)
      MotionEvent.ACTION_MOVE -> for (i in 0 until event.pointerCount) {
        move(event.getX(i), event.getY(i), event.getPointerId(i))
      }
      MotionEvent.ACTION_POINTER_UP -> up(pointer)
      MotionEvent.ACTION_UP -> {
        up(pointer)
        releaseAll() // the last finger: nothing can still be held
      }
      MotionEvent.ACTION_CANCEL -> releaseAll()
    }
  }

  /** The control a touch at (x, y) lands on: the nearest in reach, by the share of its reach. */
  private fun hit(x: Float, y: Float): ControlId? {
    val l = layout ?: return null
    var best: ControlId? = null
    var bestScore = Float.MAX_VALUE
    for (s in l.controls.values) {
      if (off(s)) continue
      val reach = s.radius * ControlPainter.reach(s.id.kind)
      val score = hypot(x - s.cx, y - s.cy) / reach
      if (score <= 1f && score < bestScore) {
        best = s.id
        bestScore = score
      }
    }
    return best
  }

  private fun down(x: Float, y: Float, pointer: Int) {
    val id = hit(x, y) ?: return
    when (id.kind) {
      ControlKind.MENU -> if (menuPointer == -1) {
        menuPointer = pointer
        menuPressed = true
      }
      ControlKind.STICK -> {
        val i = stickIndex(id)
        if (stickPointer[i] == -1) {
          stickPointer[i] = pointer
          moveStick(i, x, y)
        }
      }
      ControlKind.DPAD -> if (dpadPointer == -1) {
        dpadPointer = pointer
        moveDpad(x, y)
      }
      else -> if (id !in held) press(id, pointer)
    }
  }

  private fun move(x: Float, y: Float, pointer: Int) {
    val l = layout ?: return
    if (pointer == menuPointer) {
      val s = l[ControlId.MENU]
      menuPressed = hypot(x - s.cx, y - s.cy) <= s.radius * ControlPainter.reach(ControlKind.MENU)
      return
    }
    for (i in 0..1) {
      if (stickPointer[i] == pointer) {
        moveStick(i, x, y)
        return
      }
    }
    if (pointer == dpadPointer) {
      moveDpad(x, y)
      return
    }
    val id = held.entries.firstOrNull { it.value == pointer }?.key ?: return
    val s = l[id]
    if (hypot(x - s.cx, y - s.cy) <= s.radius * ControlPainter.reach(id.kind)) {
      return
    }
    // Sliding off a button releases it; onto another free one presses that (A to B and so on).
    release(id)
    val next = hit(x, y) ?: return
    if (next.kind != ControlKind.STICK && next.kind != ControlKind.DPAD && next.kind != ControlKind.MENU &&
      next !in held
    ) {
      press(next, pointer)
    }
  }

  private fun up(pointer: Int) {
    if (pointer == menuPointer) {
      if (menuPressed) {
        onMenuButtonTapped?.invoke()
      }
      menuPressed = false
      menuPointer = -1
    }
    for (i in 0..1) {
      if (stickPointer[i] == pointer) {
        releaseStick(i)
      }
    }
    if (pointer == dpadPointer) {
      dpadPointer = -1
      setDpad(0)
    }
    held.entries.filter { it.value == pointer }.map { it.key }.forEach(::release)
  }

  private fun releaseAll() {
    swipeUp.reset()
    menuPressed = false
    menuPointer = -1
    for (i in 0..1) {
      if (stickPointer[i] != -1) releaseStick(i)
    }
    dpadPointer = -1
    setDpad(0)
    held.keys.toList().forEach(::release)
  }

  fun resetAllInputs() {
    releaseAll()
    invalidate()
  }

  override fun onDetachedFromWindow() {
    removeCallbacks(retryRedraw)
    wantedSince = 0L
    resetAllInputs()
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
      nodes.values.forEach { it.discardDisplayList() }
    }
    nodes.clear()
    looks.clear()
    super.onDetachedFromWindow()
  }

  override fun onVisibilityChanged(changedView: View, visibility: Int) {
    super.onVisibilityChanged(changedView, visibility)
    if (changedView === this && visibility != VISIBLE) {
      resetAllInputs()
    }
  }

  private fun press(id: ControlId, pointer: Int) {
    held[id] = pointer
    buttonOf(id)?.let { listener?.onButtonPressed(it) }
  }

  private fun release(id: ControlId) {
    if (held.remove(id) != null) {
      buttonOf(id)?.let { listener?.onButtonReleased(it) }
    }
  }

  private fun moveStick(i: Int, x: Float, y: Float) {
    val s = layout?.get(if (i == 0) ControlId.LEFT_STICK else ControlId.RIGHT_STICK) ?: return
    var dx = (x - s.cx) / s.radius
    var dy = (y - s.cy) / s.radius
    val m = hypot(dx, dy)
    if (m > 1f) {
      dx /= m
      dy /= m
    }
    if (m < STICK_DEAD_ZONE) {
      dx = 0f
      dy = 0f
    }
    stickPos[i].set(dx, dy)
    listener?.onStickMoved(if (i == 0) Stick.LEFT else Stick.RIGHT, dx, dy)
  }

  private fun releaseStick(i: Int) {
    stickPointer[i] = -1
    stickPos[i].set(0f, 0f)
    listener?.onStickMoved(if (i == 0) Stick.LEFT else Stick.RIGHT, 0f, 0f)
  }

  /* Eight ways: 45-degree sectors around the axes; the middle is neutral. */
  private fun moveDpad(x: Float, y: Float) {
    val s = layout?.get(ControlId.DPAD) ?: return
    val dx = x - s.cx
    val dy = y - s.cy
    var bits = 0
    if (hypot(dx, dy) > s.radius * 0.22f) {
      val deg = Math.toDegrees(atan2(dy, dx).toDouble()).toFloat() // 0 right, 90 down
      val sector = (((deg + 360f + 22.5f) % 360f) / 45f).toInt()
      bits = when (sector) {
        0 -> ControlPainter.DPAD_RIGHT
        1 -> ControlPainter.DPAD_RIGHT or ControlPainter.DPAD_DOWN
        2 -> ControlPainter.DPAD_DOWN
        3 -> ControlPainter.DPAD_DOWN or ControlPainter.DPAD_LEFT
        4 -> ControlPainter.DPAD_LEFT
        5 -> ControlPainter.DPAD_LEFT or ControlPainter.DPAD_UP
        6 -> ControlPainter.DPAD_UP
        else -> ControlPainter.DPAD_UP or ControlPainter.DPAD_RIGHT
      }
    }
    setDpad(bits)
  }

  private fun setDpad(bits: Int) {
    val changed = bits xor dpadBits
    if (changed == 0) return
    for ((bit, button) in DPAD_BUTTONS) {
      if (changed and bit != 0) {
        if (bits and bit != 0) listener?.onButtonPressed(button) else listener?.onButtonReleased(button)
      }
    }
    dpadBits = bits
  }

  private fun stickIndex(id: ControlId) = if (id == ControlId.LEFT_STICK) 0 else 1

  private fun buttonOf(id: ControlId): Button? = when (id) {
    ControlId.A -> Button.A
    ControlId.B -> Button.B
    ControlId.X -> Button.X
    ControlId.Y -> Button.Y
    ControlId.WHITE -> Button.WHITE
    ControlId.BLACK -> Button.BLACK
    ControlId.BACK -> Button.BACK
    ControlId.START -> Button.START
    ControlId.LEFT_TRIGGER -> Button.LEFT_TRIGGER
    ControlId.RIGHT_TRIGGER -> Button.RIGHT_TRIGGER
    ControlId.LEFT_STICK_CLICK -> Button.LEFT_STICK_BUTTON
    ControlId.RIGHT_STICK_CLICK -> Button.RIGHT_STICK_BUTTON
    else -> null
  }

  // ---------------------------------------------------------------- editing

  private fun editTouch(event: MotionEvent) {
    val l = layout ?: return
    val index = event.actionIndex
    when (event.actionMasked) {
      MotionEvent.ACTION_DOWN -> {
        val x = event.getX(index)
        val y = event.getY(index)
        dragPointer = event.getPointerId(index)
        val sel = selected
        if (sel != null && onHandle(l[sel], x, y)) {
          dragMode = Drag.RESIZE
          return
        }
        val id = editHit(x, y)
        select(id)
        if (id != null) {
          dragMode = Drag.MOVE
          dragOffset.set(l[id].cx - x, l[id].cy - y)
        } else {
          dragMode = Drag.NONE
        }
      }
      MotionEvent.ACTION_POINTER_DOWN -> {
        val sel = selected ?: return
        if (event.pointerCount == 2) {
          pinchStart = hypot(event.getX(0) - event.getX(1), event.getY(0) - event.getY(1))
          pinchStartRadius = l[sel].radius
          dragMode = Drag.PINCH
        }
      }
      MotionEvent.ACTION_MOVE -> {
        val sel = selected ?: return
        val s = l[sel]
        when (dragMode) {
          Drag.MOVE -> {
            val i = event.findPointerIndex(dragPointer)
            if (i < 0) return
            s.cx = (event.getX(i) + dragOffset.x).coerceIn(0f, width.toFloat())
            s.cy = (event.getY(i) + dragOffset.y).coerceIn(0f, height.toFloat())
            onLayoutChanged?.invoke()
          }
          Drag.RESIZE -> {
            val i = event.findPointerIndex(dragPointer)
            if (i < 0) return
            val b = max(abs(event.getX(i) - s.cx), abs(event.getY(i) - s.cy))
            setRadius(s, b / ControlPainter.reach(s.id.kind))
          }
          Drag.PINCH -> if (event.pointerCount >= 2 && pinchStart > 0f) {
            val d = hypot(event.getX(0) - event.getX(1), event.getY(0) - event.getY(1))
            setRadius(s, pinchStartRadius * d / pinchStart)
          }
          Drag.NONE -> {}
        }
      }
      MotionEvent.ACTION_POINTER_UP -> if (dragMode == Drag.PINCH) {
        dragMode = Drag.NONE
      }
      MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
        dragMode = Drag.NONE
        dragPointer = -1
      }
    }
  }

  /* Editing reaches hidden controls too, so they can be shown again. */
  private fun editHit(x: Float, y: Float): ControlId? {
    val l = layout ?: return null
    var best: ControlId? = null
    var bestScore = Float.MAX_VALUE
    for (s in l.controls.values) {
      val reach = s.radius * ControlPainter.reach(s.id.kind)
      val score = max(abs(x - s.cx), abs(y - s.cy)) / reach
      if (score <= 1f && score < bestScore) {
        best = s.id
        bestScore = score
      }
    }
    return best
  }

  private fun onHandle(s: ControlSpec, x: Float, y: Float): Boolean {
    val b = s.radius * ControlPainter.reach(s.id.kind)
    return hypot(x - (s.cx + b), y - (s.cy + b)) <= handleRadius() * 1.6f
  }

  private fun setRadius(s: ControlSpec, r: Float) {
    s.radius = r.coerceIn(OverlayStore.minRadius(height.toFloat()), OverlayStore.maxRadius(height.toFloat()))
    onLayoutChanged?.invoke()
  }

  private fun select(id: ControlId?) {
    if (selected != id) {
      selected = id
      onSelectionChanged?.invoke(id)
    }
  }

  /** The control being edited, or null. */
  fun selectedControl(): ControlId? = selected

  /**
   * The selected control's size against its default, in percent; none
   * selected, every control's if they share one, else null (they differ).
   */
  fun scalePercent(): Int? {
    val l = layout ?: return null
    val d = defaults ?: return null
    fun percent(id: ControlId) = (l[id].radius / d[id].radius * 100f).roundToInt()
    val id = selected
    if (id != null) {
      return percent(id)
    }
    return l.controls.keys.map(::percent).distinct().singleOrNull()
  }

  fun isSelectedVisible(): Boolean = selected?.let { layout?.get(it)?.visible } ?: true

  /** Grows or shrinks the selected control (none selected: every control, each in place). */
  fun resize(factor: Float) {
    val l = layout ?: return
    val targets = selected?.let { listOf(l[it]) } ?: l.controls.values
    for (s in targets) {
      setRadius(s, s.radius * factor)
    }
    invalidate()
  }

  /** Hides the selected control, or shows it again (the jewel always stays: it opens the menu). */
  fun toggleSelectedVisible() {
    val l = layout ?: return
    val id = selected ?: return
    if (id == ControlId.MENU) return
    l[id].visible = !l[id].visible
    onLayoutChanged?.invoke()
    invalidate()
  }

  /**
   * The selected control's opacity in percent; none selected, every
   * control's if they share one, else null (they differ).
   */
  fun opacityPercent(): Int? {
    val l = layout ?: return null
    val id = selected
    if (id != null) {
      return (l[id].opacity * 100f).roundToInt()
    }
    val all = l.controls.values.map { (it.opacity * 100f).roundToInt() }.distinct()
    return all.singleOrNull()
  }

  /** Makes the selected control (none selected: every control) more or less see-through. */
  fun changeOpacity(delta: Float) {
    val l = layout ?: return
    val targets = selected?.let { listOf(l[it]) } ?: l.controls.values
    for (s in targets) {
      // In whole steps, so the controls changed together stay equal.
      val v = ((s.opacity + delta) * 10f).roundToInt() / 10f
      s.opacity = v.coerceIn(OverlayStore.MIN_OPACITY, 1f)
    }
    onLayoutChanged?.invoke()
    invalidate()
  }

  /** Keeps the layout for the next game and the next edit. */
  fun saveLayout() {
    val l = layout ?: return
    OverlayStore.save(context, l, width.toFloat(), height.toFloat())
  }

  /** The default layout (kept once saved). */
  fun resetLayout() {
    if (width == 0 || height == 0) return
    layout = OverlayStore.defaults(width.toFloat(), height.toFloat())
    select(null)
    onLayoutChanged?.invoke()
    invalidate()
  }

  /** Drops unsaved edits. */
  fun reloadLayout() {
    if (width == 0 || height == 0) return
    layout = OverlayStore.load(context, width.toFloat(), height.toFloat())
    select(null)
    onLayoutChanged?.invoke()
    invalidate()
  }

  private companion object {
    const val STICK_DEAD_ZONE = 0.15f
    val ACCENT = 0xFF8BC34A.toInt()
    val DPAD_BUTTONS = listOf(
      ControlPainter.DPAD_UP to Button.DPAD_UP,
      ControlPainter.DPAD_DOWN to Button.DPAD_DOWN,
      ControlPainter.DPAD_LEFT to Button.DPAD_LEFT,
      ControlPainter.DPAD_RIGHT to Button.DPAD_RIGHT,
    )
  }
}
