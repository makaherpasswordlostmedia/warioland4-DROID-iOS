/* Tiny freestanding runtime for the wasm32 module (compiled with -mbulk-memory). */
typedef unsigned int size_t_;
void *memcpy(void *d, const void *s, size_t_ n) { __builtin_memcpy(d, s, n); return d; }
void *memmove(void *d, const void *s, size_t_ n) { __builtin_memmove(d, s, n); return d; }
void *memset(void *d, int c, size_t_ n) { __builtin_memset(d, c, n); return d; }
/* ARM libgcc helpers the decomp links against; GBA BIOS Div returns garbage-but-defined for /0 */
int __divsi3(int a, int b) { return b ? (a == (-2147483647 - 1) && b == -1 ? a : a / b) : (a < 0 ? -1 : 1); }
int __modsi3(int a, int b) { return b ? (b == -1 ? 0 : a % b) : a; }
unsigned __udivsi3(unsigned a, unsigned b) { return b ? a / b : 0xFFFFFFFFu; }
unsigned __umodsi3(unsigned a, unsigned b) { return b ? a % b : a; }
