/* Headless mGBA test harness for STAK! GBA.
 * Usage: harness ROM SCRIPT
 * Script commands (one per line, # comments):
 *   wait N            run N frames holding current keys
 *   hold KEYS         set held keys (e.g. "A", "UP+A", "-" for none)
 *   tap KEYS [N]      hold KEYS for N frames (default 2), then release for 2
 *   shot FILE.ppm     write current frame
 *   audio FILE.wav    start capturing audio (written at exit)
 *   mark TEXT         print a marker
 * mGBA debug-log output (REG_DEBUG) is printed to stdout.
 */
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/blip_buf.h>
#include <mgba/gba/interface.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static struct mCore* core;
static uint32_t* fb;
static FILE* wav; static long wavSamples;
static int16_t abuf[4096*2];

static void logfn(struct mLogger* l, int cat, enum mLogLevel lvl, const char* fmt, va_list a) {
  (void)l;
  const char* c = mLogCategoryName(cat);
  if (!c || strcmp(c, "GBA Debug")) return; /* only game prints */
  (void)lvl;
  printf("[gba] "); vprintf(fmt, a); printf("\n");
}
static struct mLogger logger = { .log = logfn };

static uint32_t parseKeys(const char* s) {
  uint32_t k = 0; char buf[128]; strncpy(buf, s, 127); buf[127]=0;
  for (char* t = strtok(buf, "+"); t; t = strtok(NULL, "+")) {
    if (!strcmp(t,"A")) k|=1; else if(!strcmp(t,"B")) k|=2; else if(!strcmp(t,"SELECT")) k|=4;
    else if(!strcmp(t,"START")) k|=8; else if(!strcmp(t,"RIGHT")) k|=16; else if(!strcmp(t,"LEFT")) k|=32;
    else if(!strcmp(t,"UP")) k|=64; else if(!strcmp(t,"DOWN")) k|=128; else if(!strcmp(t,"R")) k|=256;
    else if(!strcmp(t,"L")) k|=512;
  }
  return k;
}
static void frame(void) {
  core->runFrame(core);
  blip_t* L = core->getAudioChannel(core, 0); blip_t* R = core->getAudioChannel(core, 1);
  int n = blip_samples_avail(L);
  while (n > 0) {
    int c = n > 4096 ? 4096 : n;
    blip_read_samples(L, abuf, c, 1); blip_read_samples(R, abuf+1, c, 1);
    if (wav) { fwrite(abuf, 4, c, wav); wavSamples += c; }
    n -= c;
  }
}
static void shot(const char* path) {
  FILE* f = fopen(path, "wb"); fprintf(f, "P6\n240 160\n255\n");
  for (int i = 0; i < 240*160; i++) { uint32_t p = fb[i]; unsigned char rgb[3] = { p & 0xFF, (p>>8)&0xFF, (p>>16)&0xFF }; fwrite(rgb,1,3,f); }
  fclose(f);
}
static void wavhdr(FILE* f, long n) {
  uint32_t r=32768, br=r*4, ds=n*4, rs=36+ds; uint16_t fmt=1,ch=2,ba=4,bps=16; uint32_t sz=16;
  fseek(f,0,SEEK_SET); fwrite("RIFF",1,4,f); fwrite(&rs,4,1,f); fwrite("WAVEfmt ",1,8,f); fwrite(&sz,4,1,f);
  fwrite(&fmt,2,1,f); fwrite(&ch,2,1,f); fwrite(&r,4,1,f); fwrite(&br,4,1,f); fwrite(&ba,2,1,f); fwrite(&bps,2,1,f);
  fwrite("data",1,4,f); fwrite(&ds,4,1,f);
}
int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: harness ROM SCRIPT\n"); return 1; }
  mLogSetDefaultLogger(&logger);
  core = mCoreFind(argv[1]);
  if (!core || !core->init(core)) { fprintf(stderr, "no core\n"); return 1; }
  mCoreInitConfig(core, NULL);
  fb = calloc(256*256, 4);
  core->setVideoBuffer(core, (color_t*)fb, 240);
  core->setAudioBufferSize(core, 4096);
  blip_set_rates(core->getAudioChannel(core,0), core->frequency(core), 32768);
  blip_set_rates(core->getAudioChannel(core,1), core->frequency(core), 32768);
  if (!mCoreLoadFile(core, argv[1])) { fprintf(stderr, "load failed\n"); return 1; }
  core->reset(core);
  FILE* s = fopen(argv[2], "r"); char line[512]; uint32_t keys = 0;
  while (fgets(line, sizeof line, s)) {
    char cmd[64], a1[256] = "", a2[64] = "";
    if (line[0]=='#' || sscanf(line, "%63s %255s %63s", cmd, a1, a2) < 1) continue;
    if (!strcmp(cmd,"wait")) { int n = atoi(a1); for (int i=0;i<n;i++) frame(); }
    else if (!strcmp(cmd,"hold")) { keys = parseKeys(a1); core->setKeys(core, keys); }
    else if (!strcmp(cmd,"tap")) { int n = a2[0]?atoi(a2):2; core->setKeys(core, keys|parseKeys(a1)); for(int i=0;i<n;i++) frame(); core->setKeys(core, keys); frame(); frame(); }
    else if (!strcmp(cmd,"shot")) shot(a1);
    else if (!strcmp(cmd,"audio")) { wav = fopen(a1,"wb"); wavhdr(wav,0); }
    else if (!strcmp(cmd,"mark")) printf("[mark] %s @%u\n", line+5, core->frameCounter(core));
  }
  if (wav) { wavhdr(wav, wavSamples); fclose(wav); }
  core->deinit(core);
  return 0;
}
