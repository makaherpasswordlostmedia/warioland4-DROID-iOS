/* Native (x86, -m32) test stand for the m4a sequencer + mixer in port/rt/rt.c.
 *
 * Maps your ROM at 0x08000000, runs the *portified* src/m4a.c + m4a_tables.c + port/rt/rt.c against it, starts every song in
 * the ROM's song table for 10 s and reports peak / RMS of the mixed PCM.  No libc, raw Linux syscalls.
 *
 *   python3 port/tools/portify.py . /tmp/p
 *   F="-m32 -O1 -w -ffreestanding -fno-builtin -fno-stack-protector -fno-pie -mno-sse -include types.h -I/tmp/p/include -I. -Iport/include"
 *   gcc $F -c /tmp/p/src/m4a.c -o /tmp/m4a.o; gcc $F -c /tmp/p/src/m4a_tables.c -o /tmp/m4a_tables.o; gcc $F -c port/rt/rt.c -o /tmp/rt.o
 *   gcc $F -c port/tests/m4a_native.c -o /tmp/t.o
 *   gcc -m32 -nostdlib -static -no-pie -Wl,-Ttext-segment=0x40000000 -Wl,--allow-multiple-definition -Wl,-e,_start \
 *       -Wl,--defsym=gMPlayTable=0x08097FC8 -Wl,--defsym=gSongTable=0x08098028 /tmp/t.o /tmp/m4a.o /tmp/m4a_tables.o /tmp/rt.o -o /tmp/m4a_native
 *   /tmp/m4a_native      (edit the ROM path in _start)
 */
/* native -m32 test stand: real ROM mapped at 0x08000000, portified m4a.c + rt.c mixer/sequencer, no libc */
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef signed char s8;
static inline int sys3(int n,int a,int b,int c){int r;__asm__ volatile("int $0x80":"=a"(r):"0"(n),"b"(a),"c"(b),"d"(c):"memory");return r;}
static inline int sys6(int n,int a,int b,int c,int d,int e,int f){int r;__asm__ volatile("push %%ebp; mov %7,%%ebp; int $0x80; pop %%ebp":"=a"(r):"0"(n),"b"(a),"c"(b),"d"(c),"S"(d),"D"(e),"m"(f):"memory");return r;}
static void out(const char*s){int n=0;while(s[n])n++;sys3(4,1,(int)s,n);}
static void outn(const char*p,int v){out(p);char b[16];int i=15;b[i]=0;int neg=v<0;if(neg)v=-v;do{b[--i]='0'+v%10;v/=10;}while(v);if(neg)b[--i]='-';out(&b[i]);}
void *memcpy(void*d,const void*s,u32 n){u8*a=d;const u8*b=s;while(n--)*a++=*b++;return d;}
void *memset(void*d,int c,u32 n){u8*a=d;while(n--)*a++=c;return d;}
void *memmove(void*d,const void*s,u32 n){u8*a=d;const u8*b=s;if(a<b)while(n--)*a++=*b++;else{a+=n;b+=n;while(n--)*--a=*--b;}return d;}
/* hal / bios stubs the portified m4a.c needs */
static u32 vc; unsigned hal_poll_vcount(void){return vc++%228;}
void CPUSet(const void*src_,void*dst_,u32 ctl){u32 src=(u32)src_,dst=(u32)dst_;u32 n=ctl&0x1FFFFF;int fill=ctl&(1<<24),w32=ctl&(1<<26);
 for(u32 i=0;i<n;i++){ if(w32){*(u32*)dst=*(u32*)src;dst+=4;if(!fill)src+=4;} else {*(u16*)dst=*(u16*)src;dst+=2;if(!fill)src+=2;} } }
void CpuFastSet(const void*src,void*dst,u32 ctl){CPUSet(src,dst,(ctl&0x1FFFFF)|(ctl&(1<<24))|(1<<26));}
void hal_dma_set(int ch,u32 s,u32 d,u32 c){} void hal_unported_asm(u32 s){} void hal_trace_val(u32 a,u32 b){}
char SoundMainRAM[0x400]; void hal_syscall(u32 n){}
long long __divdi3(long long a,long long b){int neg=0;if(a<0){a=-a;neg^=1;}if(b<0){b=-b;neg^=1;}unsigned long long q=0,r=0,n=a;for(int i=63;i>=0;i--){r=(r<<1)|((n>>i)&1);if(r>=(unsigned long long)b){r-=b;q|=1ULL<<i;}}return neg?-(long long)q:(long long)q;}
/* PCM capture */
#define MAXS (13379*30)
static s8 capR[MAXS], capL[MAXS]; static int ncap, rateSeen;
void hal_audio_push(const void*r,const void*l,int n,int rate){rateSeen=rate;for(int i=0;i<n&&ncap<MAXS;i++){capR[ncap]=((const s8*)r)[i];capL[ncap]=((const s8*)l)[i];ncap++;}}
static long isq(long long v){long long x=v,y=(x+1)/2;if(!v)return 0;while(y<x){x=y;y=(x+v/x)/2;}return x;}
void m4aSoundInit(void); void m4aSoundMain(void); void m4aSoundVSync(void); void m4aSongNumStart(u16 n);
void m4aMPlayAllStop(void);
static void *mapfixed(u32 addr,u32 len,int prot,int flags,int fd,int off){u32 a[6]={addr,len,(u32)prot,(u32)(flags|0x10),(u32)fd,(u32)off};return (void*)sys3(90,(int)a,0,0);}
__attribute__((force_align_arg_pointer)) void _start(void){
  int fd=sys3(5,(int)"/mnt/user-data/uploads/Wario_Land_4__USA__Europe_.gba",0,0);
  outn("fd=",fd);out("\n");
  if(fd<0){out("open fail\n");sys3(1,2,0,0);}
  if((int)mapfixed(0x08000000,0x800000,1,2,fd,0)<0){out("map rom fail\n");sys3(1,3,0,0);}
  mapfixed(0x03000000,0x8000,3,0x22,-1,0); mapfixed(0x04000000,0x1000,3,0x22,-1,0); mapfixed(0x02000000,0x40000,3,0x22,-1,0);
  mapfixed(0x06000000,0x20000,3,0x22,-1,0);
  out("mapped\n"); outn("iwram=",*(int*)0x03000000);out("\n");
  m4aSoundInit(); out("init done\n");
  outn("rate=",rateSeen);out("\n");
  int tested=0,audible=0;
  for(int song=0;song<0x100;song++){
    u32 hdr=(u32)gSongTable[song].header; if(hdr<0x08000000||hdr>=0x08800000) continue;
    ncap=0; m4aSoundMain(); /* flush */ ncap=0;
    m4aSongNumStart(song);
    for(int f=0;f<600;f++){ m4aSoundVSync(); m4aSoundMain(); }
    long peak=0; long long sumsq=0; int nz=0; for(int i=0;i<ncap;i++){int a=capR[i]<0?-capR[i]:capR[i];int b=capL[i]<0?-capL[i]:capL[i];if(a>peak)peak=a;if(b>peak)peak=b;sumsq+=capR[i]*capR[i]+capL[i]*capL[i];if(capR[i]||capL[i])nz++;}
    tested++; if(peak>8)audible++;
    if(nz*100/(ncap?ncap:1)>60||peak>=60){outn("song ",song);outn(" samples=",ncap);outn(" peak=",(int)peak);outn(" rms=",(int)isq(sumsq/(ncap*2+1)));outn(" nonzero%=",ncap?nz*100/ncap:0);out("\n");}
    m4aMPlayAllStop();
    for(int f=0;f<30;f++){ m4aSoundVSync(); m4aSoundMain(); }
  }
  outn("tested=",tested);outn(" audible=",audible);out("\n");
  sys3(1,0,0,0);
}
