/* Declarations of host (HAL) functions imported by the wasm module. */
#ifndef PORT_H
#define PORT_H
#ifdef __wasm__
#define PORT_IMPORT(name) __attribute__((import_module("env"), import_name(name)))
PORT_IMPORT("hal_poll_vcount") unsigned short hal_poll_vcount(void);
PORT_IMPORT("hal_unported_asm") void hal_unported_asm(int site);
PORT_IMPORT("hal_trace_val") void hal_trace_val(int tag, unsigned val);
PORT_IMPORT("hal_syscall") void hal_syscall(int num);
PORT_IMPORT("hal_dma_set") void hal_dma_set(int ch, unsigned src, unsigned dst, unsigned cntctl);
#endif
#endif
