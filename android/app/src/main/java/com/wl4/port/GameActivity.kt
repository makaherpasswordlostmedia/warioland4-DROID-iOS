package com.wl4.port

import android.graphics.*
import android.os.Bundle
import android.view.*
import androidx.appcompat.app.AppCompatActivity
import java.io.File

class GameActivity : AppCompatActivity() {
    private lateinit var view: GameView
    private val saveFile get() = File(filesDir, "wl4.sav")
    private var keysPad = 0; private var keysTouch = 0

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        view = GameView(this) { keysTouch = it; push() }
        setContentView(view)
        val rom = File(filesDir, "wl4.gba").takeIf { it.exists() }?.readBytes()
            ?: run { Native.log("ROM not found: ${File(filesDir, "wl4.gba")} - closing"); finish(); return }
        Native.log("GameActivity: rom ${rom.size} bytes, running=${Native.running()}")
        val save = saveFile.takeIf { it.exists() }?.readBytes()
        if (!Native.running()) Native.log("Native.start returned ${Native.start(rom, save)}")
    }
    private fun push() = Native.setKeys(keysPad or keysTouch)

    override fun onPause() { super.onPause(); Native.pause(true); saveFile.writeBytes(Native.readSave()) }
    override fun onResume() { super.onResume(); Native.pause(false); view.start() }
    override fun onDestroy() { super.onDestroy(); view.stop(); if (isFinishing) { saveFile.writeBytes(Native.readSave()); Native.stop() } }

    private fun mapKey(k: Int) = when (k) {
        KeyEvent.KEYCODE_BUTTON_A -> Key.A; KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_X -> Key.B
        KeyEvent.KEYCODE_BUTTON_START -> Key.START; KeyEvent.KEYCODE_BUTTON_SELECT -> Key.SELECT
        KeyEvent.KEYCODE_BUTTON_R1 -> Key.R; KeyEvent.KEYCODE_BUTTON_L1 -> Key.L
        KeyEvent.KEYCODE_DPAD_UP -> Key.UP; KeyEvent.KEYCODE_DPAD_DOWN -> Key.DOWN
        KeyEvent.KEYCODE_DPAD_LEFT -> Key.LEFT; KeyEvent.KEYCODE_DPAD_RIGHT -> Key.RIGHT
        KeyEvent.KEYCODE_Z -> Key.A; KeyEvent.KEYCODE_X -> Key.B; KeyEvent.KEYCODE_ENTER -> Key.START
        else -> 0 }
    override fun dispatchKeyEvent(e: KeyEvent): Boolean {
        val m = mapKey(e.keyCode); if (m == 0) return super.dispatchKeyEvent(e)
        keysPad = if (e.action == KeyEvent.ACTION_DOWN) keysPad or m else keysPad and m.inv(); push(); return true
    }
    override fun dispatchGenericMotionEvent(e: MotionEvent): Boolean {
        if (e.source and InputDevice.SOURCE_JOYSTICK == 0) return super.dispatchGenericMotionEvent(e)
        val x = e.getAxisValue(MotionEvent.AXIS_X) + e.getAxisValue(MotionEvent.AXIS_HAT_X)
        val y = e.getAxisValue(MotionEvent.AXIS_Y) + e.getAxisValue(MotionEvent.AXIS_HAT_Y)
        var k = keysPad and (Key.LEFT or Key.RIGHT or Key.UP or Key.DOWN).inv()
        if (x < -0.5f) k = k or Key.LEFT; if (x > 0.5f) k = k or Key.RIGHT
        if (y < -0.5f) k = k or Key.UP; if (y > 0.5f) k = k or Key.DOWN
        keysPad = k; push(); return true
    }
}

/** Draws the 240x160 frame letterboxed at integer-ish scale and an on-screen GBA pad. */
class GameView(ctx: android.content.Context, private val onTouchKeys: (Int) -> Unit) : SurfaceView(ctx), SurfaceHolder.Callback, Runnable {
    private val px = IntArray(240 * 160); private val bmp = Bitmap.createBitmap(240, 160, Bitmap.Config.ARGB_8888)
    private val paint = Paint(); private val pad = Paint().apply { color = 0x66FFFFFF; style = Paint.Style.FILL }
    private val txt = Paint().apply { color = Color.BLACK; textSize = 40f; textAlign = Paint.Align.CENTER }
    @Volatile private var go = false; private var th: Thread? = null; private var last = -1
    private class Btn(val key: Int, val label: String, var cx: Float = 0f, var cy: Float = 0f, var r: Float = 0f)
    private val btns = listOf(Btn(Key.UP, "▲"), Btn(Key.DOWN, "▼"), Btn(Key.LEFT, "◀"), Btn(Key.RIGHT, "▶"),
        Btn(Key.A, "A"), Btn(Key.B, "B"), Btn(Key.L, "L"), Btn(Key.R, "R"), Btn(Key.START, "St"), Btn(Key.SELECT, "Se"))
    init { holder.addCallback(this); paint.isFilterBitmap = false }

    override fun surfaceCreated(h: SurfaceHolder) = start()
    override fun surfaceDestroyed(h: SurfaceHolder) = stop()
    override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, hh: Int) {
        val u = minOf(w, hh) / 7f
        fun set(k: Int, x: Float, y: Float, r: Float = u * 0.8f) = btns.first { it.key == k }.apply { cx = x; cy = y; this.r = r }
        set(Key.UP, u * 1.6f, hh - u * 3.6f); set(Key.DOWN, u * 1.6f, hh - u * 1.2f)
        set(Key.LEFT, u * 0.4f + u * 0.4f, hh - u * 2.4f); set(Key.RIGHT, u * 2.8f, hh - u * 2.4f)
        set(Key.A, w - u * 1.2f, hh - u * 2.6f); set(Key.B, w - u * 2.8f, hh - u * 1.4f)
        set(Key.L, u * 1.0f, u * 0.8f); set(Key.R, w - u * 1.0f, u * 0.8f)
        set(Key.START, w / 2f + u, hh - u * 0.8f, u * 0.55f); set(Key.SELECT, w / 2f - u, hh - u * 0.8f, u * 0.55f)
    }
    fun start() { if (go) return; go = true; th = Thread(this, "present").also { it.start() } }
    fun stop() { go = false; th?.join(); th = null }

    override fun run() {
        while (go) {
            val f = Native.copyFrame(px)
            val c = holder.surface.takeIf { it.isValid }?.let { holder.lockCanvas() }
            if (c != null) {
                if (f != last) { bmp.setPixels(px, 0, 240, 0, 0, 240, 160); last = f }
                c.drawColor(Color.BLACK)
                val s = minOf(width / 240f, height / 160f); val w = 240 * s; val h = 160 * s
                c.drawBitmap(bmp, null, RectF((width - w) / 2, (height - h) / 2, (width + w) / 2, (height + h) / 2), paint)
                for (b in btns) { c.drawCircle(b.cx, b.cy, b.r, pad); c.drawText(b.label, b.cx, b.cy + 14f, txt) }
                holder.unlockCanvasAndPost(c)
            }
            try { Thread.sleep(8) } catch (_: InterruptedException) {}
        }
    }
    override fun onTouchEvent(e: MotionEvent): Boolean {
        var k = 0
        if (e.actionMasked != MotionEvent.ACTION_UP && e.actionMasked != MotionEvent.ACTION_CANCEL) {
            for (i in 0 until e.pointerCount) {
                if (e.actionMasked == MotionEvent.ACTION_POINTER_UP && i == e.actionIndex) continue
                for (b in btns) { val dx = e.getX(i) - b.cx; val dy = e.getY(i) - b.cy; if (dx * dx + dy * dy <= b.r * b.r * 1.5f) k = k or b.key }
            }
        }
        onTouchKeys(k); return true
    }
}
