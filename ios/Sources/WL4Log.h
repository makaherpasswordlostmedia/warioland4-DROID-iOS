#ifndef WL4_LOG_H
#define WL4_LOG_H
#ifdef __cplusplus
extern "C" {
#endif
/* Redirect stdout+stderr into `path` (the port's WL4_LOG/fprintf output lands there) and install a fatal-signal handler
 * that appends the flight-recorder ring before the process dies.  Also sets the stall/trap dump path (<path>.stall.bin). */
void wl4_log_open(const char *path);
#ifdef __cplusplus
}
#endif
#endif
