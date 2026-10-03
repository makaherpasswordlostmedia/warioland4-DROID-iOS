# wasm32 replacement for asm/macros.s.inc: only data blobs are assembled for the port.
.macro baserom_blob start end
    .incbin "baserom.gba", \start, (\end - \start)
.endm
