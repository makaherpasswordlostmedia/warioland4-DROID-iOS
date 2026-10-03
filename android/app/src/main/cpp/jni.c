#include <jni.h>
#include <stdlib.h>
#include "host.h"

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
