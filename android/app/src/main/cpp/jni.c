#include <jni.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <android/log.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include "host.h"

/* Android drops stdout/stderr: pipe both into logcat (tag "wl4") so printf/fprintf in the port are visible. */
static int g_logPipe[2];
static int g_logFd = -1;
static long long g_t0ms;
static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
static pthread_mutex_t g_logLock = PTHREAD_MUTEX_INITIALIZER;
/* one line -> logcat AND the log file (so it can be read on the phone without a PC) */
static void wl4_emit(const char *line) {
    __android_log_write(ANDROID_LOG_INFO, "wl4", line);
    pthread_mutex_lock(&g_logLock);
    if (g_logFd >= 0) {                                   /* file lines carry a [seconds since app start] stamp */
        char out[700]; long long ms = mono_ms() - g_t0ms;
        int n = snprintf(out, sizeof out - 1, "[%7lld.%03lld] %s\n", ms / 1000, ms % 1000, line); if (n > (int)sizeof out - 2) n = (int)sizeof out - 2;
        (void)!write(g_logFd, out, (size_t)n);
    }
    pthread_mutex_unlock(&g_logLock);
}
static void on_fatal_signal(int sig, siginfo_t *si, void *ctx) {
    (void)ctx; char m[96]; int n = snprintf(m, sizeof m, "FATAL native signal %d, fault addr %p\n", sig, si ? si->si_addr : 0);
    if (g_logFd >= 0) { (void)!write(g_logFd, m, (size_t)n); wl4_ring_write(g_logFd); }
    __android_log_write(ANDROID_LOG_FATAL, "wl4", m);
    signal(sig, SIG_DFL); raise(sig);
}
static void *log_pump(void *arg) {
    (void)arg; char buf[512]; ssize_t n; size_t len = 0;
    while ((n = read(g_logPipe[0], buf + len, sizeof buf - 1 - len)) > 0) {
        len += (size_t)n; buf[len] = 0;
        char *start = buf, *nl;
        while ((nl = strchr(start, '\n'))) { *nl = 0; wl4_emit(start); start = nl + 1; }
        len = strlen(start); memmove(buf, start, len);
        if (len >= sizeof buf - 1) { buf[len] = 0; wl4_emit(buf); len = 0; }
    }
    return NULL;
}
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)vm; (void)reserved; g_t0ms = mono_ms();
    setvbuf(stdout, NULL, _IOLBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    if (pipe(g_logPipe) == 0) {
        dup2(g_logPipe[1], STDOUT_FILENO); dup2(g_logPipe[1], STDERR_FILENO);
        pthread_t t; if (pthread_create(&t, NULL, log_pump, NULL) == 0) pthread_detach(t);
    }
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_fatal_signal; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGABRT, &sa, NULL); sigaction(SIGFPE, &sa, NULL); sigaction(SIGILL, &sa, NULL);
    wl4_emit("libwl4 loaded");
    return JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL Java_com_wl4_port_Native_start(JNIEnv *env, jclass c, jbyteArray rom, jbyteArray save) {
    jsize n = (*env)->GetArrayLength(env, rom);
    jbyte *p = (*env)->GetByteArrayElements(env, rom, NULL);
    if (save) {
        jsize sn = (*env)->GetArrayLength(env, save);
        jbyte *s = (*env)->GetByteArrayElements(env, save, NULL);
        wl4_load_save((const uint8_t *)s, (size_t)sn);
        (*env)->ReleaseByteArrayElements(env, save, s, JNI_ABORT);
    }
    int rc = wl4_start((const uint8_t *)p, (size_t)n);      /* copies the ROM */
    { char m[96]; snprintf(m, sizeof m, "Native.start: rom=%d bytes, save=%s, rc=%d", (int)n, save ? "yes" : "no", rc); wl4_emit(m); }
    (*env)->ReleaseByteArrayElements(env, rom, p, JNI_ABORT);
    return rc;
}
JNIEXPORT void JNICALL Java_com_wl4_port_Native_stop(JNIEnv *e, jclass c) { wl4_stop(); }
JNIEXPORT void JNICALL Java_com_wl4_port_Native_pause(JNIEnv *e, jclass c, jboolean p) { wl4_pause(p); }
JNIEXPORT void JNICALL Java_com_wl4_port_Native_setKeys(JNIEnv *e, jclass c, jint k) { wl4_set_keys((uint16_t)k); }
JNIEXPORT jboolean JNICALL Java_com_wl4_port_Native_running(JNIEnv *e, jclass c) { return wl4_running() ? JNI_TRUE : JNI_FALSE; }
JNIEXPORT jint JNICALL Java_com_wl4_port_Native_copyFrame(JNIEnv *env, jclass c, jintArray out) {
    static uint32_t buf[240 * 160];
    int f = wl4_copy_frame(buf);
    (*env)->SetIntArrayRegion(env, out, 0, 240 * 160, (const jint *)buf);
    return f;
}
JNIEXPORT jbyteArray JNICALL Java_com_wl4_port_Native_readSave(JNIEnv *env, jclass c) {
    static uint8_t tmp[0x10000];
    int n = wl4_read_save(tmp);
    jbyteArray a = (*env)->NewByteArray(env, n);
    (*env)->SetByteArrayRegion(env, a, 0, n, (const jbyte *)tmp);
    return a;
}

JNIEXPORT void JNICALL Java_com_wl4_port_Native_setLogPath(JNIEnv *env, jclass c, jstring path) {
    const char *p = (*env)->GetStringUTFChars(env, path, NULL);
    pthread_mutex_lock(&g_logLock);
    if (g_logFd >= 0) close(g_logFd);
    g_logFd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    pthread_mutex_unlock(&g_logLock);
    { char d[300]; snprintf(d, sizeof d, "%s.stall.bin", p); wl4_set_dump_path(d); }
    (*env)->ReleaseStringUTFChars(env, path, p);
    wl4_emit(g_logFd >= 0 ? "log file opened" : "log file open FAILED");
}
JNIEXPORT void JNICALL Java_com_wl4_port_Native_log(JNIEnv *env, jclass c, jstring msg) {
    const char *m = (*env)->GetStringUTFChars(env, msg, NULL);
    wl4_emit(m);
    (*env)->ReleaseStringUTFChars(env, msg, m);
}
