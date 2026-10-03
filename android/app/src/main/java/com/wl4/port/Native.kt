package com.wl4.port

object Native {
    init { System.loadLibrary("wl4") }
    external fun start(rom: ByteArray, save: ByteArray?): Int
    external fun stop()
    external fun pause(p: Boolean)
    external fun setKeys(keys: Int)
    external fun running(): Boolean
    external fun copyFrame(out: IntArray): Int
    external fun readSave(): ByteArray
}

/** KEYINPUT bit order. */
object Key { const val A = 1; const val B = 2; const val SELECT = 4; const val START = 8
    const val RIGHT = 16; const val LEFT = 32; const val UP = 64; const val DOWN = 128; const val R = 256; const val L = 512 }
