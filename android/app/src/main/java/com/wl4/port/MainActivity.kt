package com.wl4.port

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import java.io.File

/** Pick your own legally-obtained ROM once; it is copied into app-private storage. */
class MainActivity : AppCompatActivity() {
    private val romFile get() = File(filesDir, "wl4.gba")
    private lateinit var status: TextView

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        val col = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(48, 96, 48, 48) }
        status = TextView(this).apply { textSize = 16f }
        val pick = Button(this).apply { text = "Select ROM (.gba)"; setOnClickListener { startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { addCategory(Intent.CATEGORY_OPENABLE); type = "*/*" }, 1) } }
        val play = Button(this).apply { text = "Play"; setOnClickListener { startActivity(Intent(this@MainActivity, GameActivity::class.java)) } }
        col.addView(status); col.addView(pick); col.addView(play); setContentView(col)
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
