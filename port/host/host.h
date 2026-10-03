#ifndef WL4_HOST_H
#define WL4_HOST_H
#include <stdint.h>
#include <stddef.h>
int  wl4_start(const uint8_t *rom, size_t size);   /* 0 on success */
void wl4_stop(void);
void wl4_pause(int paused);
void wl4_set_keys(uint16_t keys);                  /* bit0 A,1 B,2 Select,3 Start,4 Right,5 Left,6 Up,7 Down,8 R,9 L */
void wl4_load_save(const uint8_t *data, size_t n);  /* call before wl4_start */
int  wl4_read_save(uint8_t *out64k);               /* current SRAM contents; persist this */
int  wl4_running(void);
int  wl4_copy_frame(uint32_t *out240x160);         /* returns frame counter */
#endif
