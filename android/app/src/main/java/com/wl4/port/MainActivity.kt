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
    private val prevLogFile get() = File(filesDir, "wl4.prev.log")
    private val prev2LogFile get() = File(filesDir, "wl4.prev2.log")
    private val saveFile get() = File(filesDir, "wl4.sav")
    private val stallDump get() = File(filesDir, "wl4.log.stall.bin")   // written natively on stall / wasm trap

    /* Newest first: current, previous, the one before.  The native side truncates wl4.log on every start of this
       activity, so sessions are rotated, but only if the game actually ran in them (opening the menu twice must not
       push the interesting log out). */
    private fun logText(): String {
        val sb = StringBuilder()
        fun add(title: String, f: File, keep: Int) { if (f.exists() && f.length() > 0) sb.append("=== $title ===\n").append(f.readText().takeLast(keep)).append("\n") }
        add("previous-2 session", prev2LogFile, 30_000)
        add("previous session", prevLogFile, 90_000)
        add("current session", logFile, 60_000)
        return if (sb.isEmpty()) "(log is empty)" else sb.toString()
    }
    private fun saveStallDump() {
        if (!stallDump.exists()) { Toast.makeText(this, "No stall dump yet", Toast.LENGTH_LONG).show(); return }
        if (Build.VERSION.SDK_INT < 29) { Toast.makeText(this, "Needs Android 10+", Toast.LENGTH_LONG).show(); return }
        val v = ContentValues().apply { put(MediaStore.Downloads.DISPLAY_NAME, "wl4_stall_${System.currentTimeMillis()}.bin"); put(MediaStore.Downloads.MIME_TYPE, "application/octet-stream") }
        val uri = contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v) ?: return
        contentResolver.openOutputStream(uri)?.use { o -> stallDump.inputStream().use { it.copyTo(o) } }
        Toast.makeText(this, "Saved to Downloads", Toast.LENGTH_LONG).show()
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


    /* ---- save export / import (64 KB raw SRAM image, same format as a .sav from an emulator) ---- */
    private fun currentSave(): ByteArray? =
        if (Native.running()) Native.readSave() else saveFile.takeIf { it.exists() }?.readBytes()
    private fun exportSave() {
        if (currentSave() == null) { Toast.makeText(this, "No save yet - play and save in-game first", Toast.LENGTH_LONG).show(); return }
        startActivityForResult(Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE); type = "application/octet-stream"; putExtra(Intent.EXTRA_TITLE, "wl4.sav")
        }, REQ_EXPORT_SAVE)
    }
    private fun importSave() {
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { addCategory(Intent.CATEGORY_OPENABLE); type = "*/*" }, REQ_IMPORT_SAVE)
    }
    private fun doExportSave(uri: Uri) {
        val bytes = currentSave() ?: return
        contentResolver.openOutputStream(uri, "wt")?.use { it.write(bytes) }
        Toast.makeText(this, "Save exported (${bytes.size / 1024} KB)", Toast.LENGTH_LONG).show()
    }
    private fun doImportSave(uri: Uri) {
        val bytes = contentResolver.openInputStream(uri)?.use { it.readBytes() } ?: return
        if (bytes.size < 0x2000 || bytes.size > 0x20000) { Toast.makeText(this, "Not a save file (${bytes.size} bytes)", Toast.LENGTH_LONG).show(); return }
        if (Native.running()) Native.stop()          // the running game would overwrite the imported file on pause
        if (saveFile.exists()) saveFile.copyTo(File(filesDir, "wl4.sav.bak"), overwrite = true)
        saveFile.writeBytes(bytes)
        Toast.makeText(this, "Save imported (${bytes.size / 1024} KB). Press Play.", Toast.LENGTH_LONG).show()
    }

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        // rotate: keep the last two sessions in which the game really started, before the native side truncates wl4.log
        if (logFile.exists() && logFile.readText().contains("game thread started")) {
            if (prevLogFile.exists()) prevLogFile.copyTo(prev2LogFile, overwrite = true)
            logFile.copyTo(prevLogFile, overwrite = true)
        }
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
        val dump = Button(this).apply { text = "Save stall dump to Downloads"; setOnClickListener { saveStallDump() } }
        val expSave = Button(this).apply { text = "Export save"; setOnClickListener { exportSave() } }
        val impSave = Button(this).apply { text = "Import save"; setOnClickListener { importSave() } }
        col.addView(status); col.addView(pick); col.addView(play); col.addView(expSave); col.addView(impSave); col.addView(share); col.addView(save); col.addView(dump); setContentView(col)
        refresh()
    }
    private fun refresh() { status.text = if (romFile.exists()) "ROM ready (${romFile.length() / 1024} KB)" else "No ROM selected.\nThe app does not ship any game data." }
    override fun onActivityResult(req: Int, res: Int, data: Intent?) {
        super.onActivityResult(req, res, data)
        val uri: Uri = data?.data ?: return
        if (res != RESULT_OK) return
        when (req) { REQ_EXPORT_SAVE -> { doExportSave(uri); return }; REQ_IMPORT_SAVE -> { doImportSave(uri); return } }
        contentResolver.openInputStream(uri)?.use { i -> val bytes = i.readBytes()
            if (bytes.size < 0x100000 || bytes.size > 0x2000000) { Toast.makeText(this, "Not a GBA ROM", Toast.LENGTH_LONG).show(); return }
            val code = String(bytes, 0xAC, 4)       // game code in header
            if (!code.startsWith("AWA")) { Toast.makeText(this, "Expected Wario Land 4 (AWAx), got $code", Toast.LENGTH_LONG).show(); return }
            romFile.writeBytes(bytes) }
        refresh()
    }
    companion object { const val REQ_EXPORT_SAVE = 2; const val REQ_IMPORT_SAVE = 3 }
}
