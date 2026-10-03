package com.wl4.port

import android.content.Intent
import android.content.ContentValues
import android.net.Uri
import android.os.Build
import android.provider.MediaStore
import android.os.Bundle
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import java.io.File

/** Pick your own legally-obtained ROM once; it is copied into app-private storage. */
class MainActivity : AppCompatActivity() {
    private val romFile get() = File(filesDir, "wl4.gba")
    private lateinit var status: TextView
    private val logFile get() = File(filesDir, "wl4.log")

    private fun logText(): String {
        val t = if (logFile.exists()) logFile.readText() else "(log is empty)"
        return t.takeLast(100_000)
    }
    private fun shareLog() {
        startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"; putExtra(Intent.EXTRA_SUBJECT, "wl4 log"); putExtra(Intent.EXTRA_TEXT, logText())
        }, "Send log"))
    }
    private fun saveLogToDownloads() {
        if (Build.VERSION.SDK_INT < 29) { Toast.makeText(this, "Use Share log", Toast.LENGTH_LONG).show(); return }
        val v = ContentValues().apply { put(MediaStore.Downloads.DISPLAY_NAME, "wl4_log_${System.currentTimeMillis()}.txt"); put(MediaStore.Downloads.MIME_TYPE, "text/plain") }
        val uri = contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v)
        if (uri == null) { Toast.makeText(this, "Could not save", Toast.LENGTH_LONG).show(); return }
        contentResolver.openOutputStream(uri)?.use { it.write(logText().toByteArray()) }
        Toast.makeText(this, "Saved to Downloads", Toast.LENGTH_LONG).show()
    }

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        Native.setLogPath(logFile.path)
        val prev = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { t, e ->
            Native.log("FATAL Java exception in ${t.name}:\n" + android.util.Log.getStackTraceString(e)); prev?.uncaughtException(t, e)
        }
        val col = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(48, 96, 48, 48) }
        status = TextView(this).apply { textSize = 16f }
        val pick = Button(this).apply { text = "Select ROM (.gba)"; setOnClickListener { startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { addCategory(Intent.CATEGORY_OPENABLE); type = "*/*" }, 1) } }
        val play = Button(this).apply { text = "Play"; setOnClickListener { startActivity(Intent(this@MainActivity, GameActivity::class.java)) } }
        val share = Button(this).apply { text = "Share log"; setOnClickListener { shareLog() } }
        val save = Button(this).apply { text = "Save log to Downloads"; setOnClickListener { saveLogToDownloads() } }
        col.addView(status); col.addView(pick); col.addView(play); col.addView(share); col.addView(save); setContentView(col)
        refresh()
    }
    private fun refresh() { status.text = if (romFile.exists()) "ROM ready (${romFile.length() / 1024} KB)" else "No ROM selected.\nThe app does not ship any game data." }
    override fun onActivityResult(req: Int, res: Int, data: Intent?) {
        super.onActivityResult(req, res, data)
        val uri: Uri = data?.data ?: return
        contentResolver.openInputStream(uri)?.use { i -> val bytes = i.readBytes()
            if (bytes.size < 0x100000 || bytes.size > 0x2000000) { Toast.makeText(this, "Not a GBA ROM", Toast.LENGTH_LONG).show(); return }
            val code = String(bytes, 0xAC, 4)       // game code in header
            if (!code.startsWith("AWA")) { Toast.makeText(this, "Expected Wario Land 4 (AWAx), got $code", Toast.LENGTH_LONG).show(); return }
            romFile.writeBytes(bytes) }
        refresh()
    }
}
