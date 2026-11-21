#include "mt7601u_bw_dpd.h"
#include "mt7601u_calibration.h"
#include "mt7601u_rf_bbp.h"
#include "mt7601u_iq_calibration.h"
#include "mt7601u_mmio.h"
#include "mt7601u_runtime.h"
#ifndef BWD_READ32
#define BWD_READ32(a) MMIO32(a)
#define BWD_WRITE32(a,v) (MMIO32(a)=(v))
#endif
#define MAC 0x411004u
#define AUX 0x411328u
#define BYPASS 0x410504u
#define SETTING 0x41050cu
#define TRY(x) do { uint32_t s_=(x); if(s_) return s_; } while(0)
struct mt7601u_bw_dpd_result mt7601u_bw_result, mt7601u_dpd_result, mt7601u_tssi_result;
struct mt7601u_dpd_state mt7601u_dpd_state;
static uint8_t bw_logged;
static uint32_t tssi_logged;
static uint8_t filters[4]; /* Original F87C..F87F order, zero-initialized. */
struct rf_saved { uint8_t bank, reg, before; };
struct context {
 struct rf_saved rf[24]; uint8_t nr, nb, bbp[16], regs[16];
 uint8_t selector, idx[2], dpd_bank, dpd_offset, modes[6], table[2][256], controls[5];
 uint32_t mac, aux, bypass, setting;
};
/* Foreground commands are serialized. Keep the rollback table off the IRQ stack. */
static struct context saved;
static void error(uint32_t *first,uint32_t next) { if(!*first) *first=next; }
static int32_t signed32(uint32_t x) { return x<=0x7fffffffu?(int32_t)x:-1-(int32_t)~x; }
static void delay(unsigned groups) { unsigned i,j; for(i=0;i<groups;i++) for(j=0;j<7;j++) __asm__ volatile("nop"); }
static void trace(uint32_t word) {
 unsigned n=BWD_READ32(MT7601U_IQ_TRACE_BASE);
 if(n<512) { BWD_WRITE32(MT7601U_IQ_TRACE_BASE+4+n*4,word); BWD_WRITE32(MT7601U_IQ_TRACE_BASE,n+1); }
}
static uint32_t iw(unsigned index,uint8_t value) { uint32_t s=mt7601u_bbp_write(158,index); return s?s:mt7601u_bbp_write(159,value); }
static uint32_t ir(unsigned index,uint8_t *value) { uint32_t s=mt7601u_bbp_write(158,index); return s?s:mt7601u_bbp_read(159,value); }
static uint32_t bw_pulse(uint8_t value) {
 uint32_t s=mt7601u_bbp_write(21,value|1); delay(10);
 uint32_t t=mt7601u_bbp_write(21,value&254); delay(10); return s?s:t;
}
static uint32_t br(unsigned reg,uint8_t value) {
 uint8_t v=0; uint32_t s=mt7601u_bbp_write(reg,value),t=mt7601u_bbp_read(reg,&v);
 return s?s:t?t:v==value?0:MT7601U_CAL_RESTORE;
}
static uint32_t rr(unsigned bank,unsigned reg,uint8_t value) {
 uint8_t v=0,mask=bank==5 && reg==4?0xbf:255;
 uint32_t s=mt7601u_rf_write(bank,reg,value),t=mt7601u_rf_read(bank,reg,&v);
 return s?s:t?t:((v^value)&mask)?MT7601U_CAL_RESTORE:0;
}
static uint32_t ri(unsigned index,uint8_t value) {
 uint8_t v=0; uint32_t s=iw(index,value),t=ir(index,&v);
 return s?s:t?t:v==value?0:MT7601U_CAL_RESTORE;
}
static uint32_t ru(unsigned bank,unsigned reg,uint8_t mask,uint8_t value,uint8_t *unused) {
 uint8_t updated; (void)unused; return mt7601u_rf_update(bank,reg,mask,value,&updated);
}
static uint32_t ready(void) {
 uint8_t v; if((BWD_READ32(0x410080u)&3)!=3 || (BWD_READ32(MAC)&3)) return MT7601U_CAL_NOT_READY;
 TRY(mt7601u_bbp_read(0,&v)); return v && v!=255?0:MT7601U_CAL_NOT_READY;
}
static uint32_t tick(struct mt7601u_bw_dpd_result *r) {
 uint32_t ctrl=BWD_READ32(0x400418u),n;
 if((BWD_READ32(0x400214u)&(1u<<12)) || BWD_READ32(0x400410u)!=78 || (ctrl&~128u)!=24) return MT7601U_CAL_TIMER;
 BWD_WRITE32(0x400400u,0x11); BWD_WRITE32(0x400418u,ctrl|128);
 for(n=0;n<100000;n++) { r->timer_last=BWD_READ32(0x400400u); if(r->timer_last&1) break; }
 BWD_WRITE32(0x400400u,0x11); BWD_WRITE32(0x400418u,ctrl);
 return n==100000?MT7601U_CAL_TIMER:0;
}
int32_t mt7601u_bw_word_sample(uint32_t word) {
 unsigned value=(word>>24)&127; return value<64?(int32_t)value:(int32_t)value-128;
}
int32_t mt7601u_dpd_metric(const uint8_t samples[11],uint8_t tail) {
 int32_t sum=0; unsigned i; for(i=0;i<11;i++) sum+=samples[i]; return sum+(int32_t)tail*55/-10;
}
uint8_t mt7601u_dpd_scale(uint8_t value,unsigned index) {
 unsigned scale=index<24?0:index<32?5:10;
 return value<=128?128:(uint8_t)(((value-128)*scale)/10+128);
}
uint32_t mt7601u_dpd_setting(int32_t temp,unsigned mode) {
 return mode?(temp>=351?0xc000000u:temp>=-250?0xa000000u:0x8000000u):
             (temp>=351?0xa000000u:temp>=-250?0x8000000u:0x4000000u);
}
uint8_t mt7601u_dpd_mode(int32_t temp,unsigned mode) {
 return mode?(temp>=351?24:temp>=-250?20:16):(temp>=351?20:temp>=-250?16:8);
}
static uint32_t capture(unsigned dpd) {
 static const uint8_t bw_rf[]={0,1,3,4,5,6,7,8,17,18,19,20,37,38,39,40,41,42,43,44,45,46,58,59};
 static const uint8_t dpd_rf[]={5,1,0,1,4,0,4,11,4,13,4,19,4,21,4,22,5,17,5,18,5,19,5,20};
 static const uint8_t bw_bbp[]={4,21,23};
 static const uint8_t dpd_bbp[]={4,21,23,27,65,66,109,110,241,242,244};
 struct context *c=&saved; unsigned i; uint32_t s=0;
 c->nr=dpd?12:24; c->nb=dpd?sizeof(dpd_bbp):sizeof(bw_bbp);
 c->mac=BWD_READ32(MAC); c->aux=BWD_READ32(AUX); c->bypass=BWD_READ32(BYPASS); c->setting=BWD_READ32(SETTING);
 for(i=0;i<c->nr;i++) { c->rf[i].bank=dpd?dpd_rf[2*i]:5; c->rf[i].reg=dpd?dpd_rf[2*i+1]:bw_rf[i]; TRY(mt7601u_rf_read(c->rf[i].bank,c->rf[i].reg,&c->rf[i].before)); }
 for(i=0;i<c->nb;i++) { c->regs[i]=dpd?dpd_bbp[i]:bw_bbp[i]; TRY(mt7601u_bbp_read(c->regs[i],&c->bbp[i])); }
 if(c->bbp[1]&1) return MT7601U_CAL_ACTIVE;
 TRY(mt7601u_bbp_read(158,&c->selector));
 s=ir(dpd?3:0,&c->idx[0]); if(!s) s=ir(dpd?0x8c:2,&c->idx[1]);
 error(&s,mt7601u_bbp_write(158,c->selector)); if(s) return s;
 if(!dpd && c->idx[0]) return MT7601U_CAL_ACTIVE;
 /* BW intentionally reproduces the Imilab search's bank-0:59 write. */
 if(!dpd) { TRY(mt7601u_rf_read(0,59,&c->dpd_offset)); return 0; }
 TRY(mt7601u_bbp_read(186,&c->dpd_bank)); TRY(mt7601u_bbp_read(187,&c->dpd_offset));
 s=mt7601u_bbp_write(4,c->bbp[0]|16);
 for(i=0;!s && i<6;i++) { s=mt7601u_bbp_write(186,i+2); if(!s) s=mt7601u_bbp_read(189,&c->modes[i]); }
 for(i=0;!s && i<512;i++) { s=mt7601u_bbp_write(186,2+i/256); if(!s) s=mt7601u_bbp_write(187,i&255); if(!s) s=mt7601u_bbp_read(188,&c->table[i/256][i%256]); }
 for(i=0;!s && i<5;i++) { s=mt7601u_bbp_write(186,0); if(!s) s=mt7601u_bbp_write(187,i); if(!s) s=mt7601u_bbp_read(188,&c->controls[i]); }
 error(&s,br(187,c->dpd_offset)); error(&s,br(186,c->dpd_bank)); error(&s,br(4,c->bbp[0])); return s;
}
static void mmio_restore(uint32_t *err) {
 BWD_WRITE32(BYPASS,0); BWD_WRITE32(SETTING,saved.setting); BWD_WRITE32(BYPASS,saved.bypass); BWD_WRITE32(AUX,saved.aux); BWD_WRITE32(MAC,saved.mac);
 if(BWD_READ32(SETTING)!=saved.setting || BWD_READ32(BYPASS)!=saved.bypass || BWD_READ32(AUX)!=saved.aux || BWD_READ32(MAC)!=saved.mac) error(err,MT7601U_CAL_RESTORE);
}
static uint32_t table_write(unsigned bank,unsigned index,uint8_t value,int verify) {
 uint8_t v=0; TRY(mt7601u_bbp_write(186,bank)); TRY(mt7601u_bbp_write(187,index)); TRY(mt7601u_bbp_write(188,value));
 if(verify) { TRY(mt7601u_bbp_read(188,&v)); if(v!=value) return MT7601U_CAL_RESTORE; } return 0;
}
static void rollback_table(uint32_t *err) {
 unsigned i; error(err,mt7601u_bbp_write(4,saved.bbp[0]|16));
 for(i=0;i<512;i++) error(err,table_write(2+i/256,i&255,saved.table[i/256][i%256],1));
 for(i=0;i<5;i++) error(err,table_write(0,i,saved.controls[i],1));
 for(i=0;i<6;i++) { error(err,mt7601u_bbp_write(186,i+2)); error(err,br(189,saved.modes[i])); }
}
static void cleanup(unsigned dpd,unsigned failed,uint32_t *err) {
 unsigned i;
 if(dpd) error(err,mt7601u_bbp_write(244,0));
 error(err,mt7601u_bbp_write(21,0));
 error(err,ri(dpd?3:0,dpd?saved.idx[0]:0)); error(err,ri(dpd?0x8c:2,dpd?saved.idx[1]:saved.idx[1]&249));
 for(i=0;i<saved.nr;i++) error(err,rr(saved.rf[i].bank,saved.rf[i].reg,saved.rf[i].before));
 for(i=0;i<saved.nb;i++) {
  uint8_t v=saved.bbp[i]; if(dpd && !failed) {
   if(saved.regs[i]==23) v=16;
   if(saved.regs[i]==241) v=4;
   if(saved.regs[i]==27 || saved.regs[i]==109 || saved.regs[i]==110 || saved.regs[i]==242 || saved.regs[i]==244) v=0;
  }
  error(err,br(saved.regs[i],v));
 }
 if(!dpd && !failed) {
  error(err,ru(5,17,128,0,0));
  error(err,rr(5,18,0)); error(err,rr(5,19,0)); error(err,rr(5,20,0));
 }
 if(failed || *err) {
  if(dpd) rollback_table(err); else error(err,rr(0,59,saved.dpd_offset));
  error(err,ri(dpd?3:0,saved.idx[0])); error(err,ri(dpd?0x8c:2,saved.idx[1]));
  for(i=0;i<saved.nb;i++) error(err,br(saved.regs[i],saved.bbp[i]));
 }
 if(dpd) { error(err,br(187,saved.dpd_offset)); error(err,br(186,saved.dpd_bank)); error(err,br(4,saved.bbp[0])); }
 error(err,br(158,saved.selector)); mmio_restore(err);
 if(!failed && *err) {
  BWD_WRITE32(MAC,0);
  if(dpd) rollback_table(err); else error(err,rr(0,59,saved.dpd_offset));
  for(i=0;i<saved.nb;i++) error(err,br(saved.regs[i],saved.bbp[i]));
  if(dpd) { error(err,br(187,saved.dpd_offset)); error(err,br(186,saved.dpd_bank)); }
  error(err,br(158,saved.selector)); mmio_restore(err);
 }
}
static uint32_t bw_sample(unsigned mode,unsigned width,unsigned code,uint8_t *raw,struct mt7601u_bw_dpd_result *r) {
 uint8_t v; unsigned i;
 TRY(bw_pulse(saved.bbp[1])); TRY(iw(0,0x82));
 for(i=0;i<21;i++) { delay(100); TRY(mt7601u_bbp_read(159,&v)); r->polls++; if(v==2) break; }
 if(i==21) return MT7601U_RF_BBP_POLL_TIMEOUT;
 TRY(ir(0x39,raw)); r->measurements++; trace(*raw|(mode<<8)|(width<<9)|(code<<16)); return 0;
}
static uint32_t bw_search(unsigned mode,struct mt7601u_bw_dpd_result *r,uint8_t result[2]) {
 unsigned width,code; uint8_t v,raw; int32_t baseline,target;
 TRY(ru(5,0,3,3,0)); TRY(ru(5,1,1,1,0));
 for(code=0;code<40;code++) { delay(100); TRY(mt7601u_rf_read(5,1,&v)); r->polls++; if(!(v&1)) break; }
 if(code==40) return MT7601U_RF_BBP_POLL_TIMEOUT;
 TRY(ru(5,0,3,1,0)); TRY(mt7601u_bbp_write(23,(saved.bbp[2]&224)|16));
 for(width=0;width<2;width++) {
  TRY(ru(5,8,4,width?4:0,0));
  TRY(mt7601u_bbp_write(21,saved.bbp[1]|1)); delay(10);
  TRY(mt7601u_bbp_write(4,(saved.bbp[0]&231)|(width?16:0))); delay(10); TRY(mt7601u_bbp_write(21,saved.bbp[1]&254));
  BWD_WRITE32(SETTING,mode?16:8); BWD_WRITE32(BYPASS,24);
  TRY(ru(5,17,128,128,0)); TRY(mt7601u_rf_write(5,18,mode?0xc1:0xf1));
  TRY(mt7601u_rf_write(5,19,mode?0x20:0x18)); TRY(mt7601u_rf_write(5,20,mode?2:0xc2));
  TRY(ru(5,3,63,mode?63:52,0)); TRY(ru(5,4,63,mode?63:52,0));
  if(mode) TRY(mt7601u_rf_write(5,5,49));
  TRY(ru(5,mode?58:6,127,0,0)); TRY(ru(5,mode?59:7,127,0,0)); delay(10);
  TRY(iw(2,saved.idx[1]&249)); TRY(bw_sample(mode,width,255,&raw,r));
  baseline=mt7601u_bw_word_sample(raw); target=mode?(width?2:9):(width?49:39); TRY(iw(2,saved.idx[1]|6));
  for(code=0;code<64;code++) {
   TRY(ru(5,mode?58:6,127,code,0));
   if(mode) { TRY(mt7601u_rf_read(5,59,&v)); TRY(mt7601u_rf_write(0,59,(v&128)+code)); }
   else TRY(ru(5,7,127,code,0));
   TRY(bw_sample(mode,width,code,&raw,r));
   if(baseline-mt7601u_bw_word_sample(raw)>target) break;
   /* Verified original LBI of the first byte of a BE uint32_t containing
    * the zero-extended BBP byte: both signed operands are zero. Preserve
    * this reference behavior, rather than invent calibrated measurements. */
  }
  result[width]=code<64?code:63;
 }
 return 0;
}
static uint32_t apply_filters(unsigned width,const uint8_t result[4]) {
 uint8_t v; unsigned i;
 for(i=0;i<4;i++) { unsigned reg=i<2?6+i:58+i-2; TRY(mt7601u_rf_read(5,reg,&v)); TRY(rr(5,reg,(v&192)+result[(i<2?0:1)+width*2])); }
 return rr(5,8,width?4:0);
}
uint32_t mt7601u_bw_calibrate(uint32_t value,unsigned shared_filter) {
 struct mt7601u_bw_dpd_result *r=&mt7601u_bw_result; uint8_t next[4],found[2]; unsigned i,mode=(value&255)!=0,width=(value&0xff00)!=0,apply=(value>>16)&1; uint32_t s;
 r->attempts++; r->valid=0; r->cleanup_status=r->measurements=r->polls=0; if(!apply) BWD_WRITE32(MT7601U_IQ_TRACE_BASE,0);
 for(i=0;i<4;i++) next[i]=filters[i];
 s=ready(); if(!s) s=capture(0); if(s) goto report;
 if(!apply && (mode || !shared_filter)) { s=bw_search(mode,r,found); if(!s) {next[mode]=found[0]; next[mode+2]=found[1];} }
 if(!s && !apply && !mode && shared_filter) { next[0]=next[1]+(next[1]<246?10:0); next[2]=next[3]+(next[3]<242?14:0); }
 if(!apply && (mode || !shared_filter)) cleanup(0,s,&r->cleanup_status);
 if(!s && !r->cleanup_status) s=apply_filters(width,next);
 if(s || r->cleanup_status) { for(i=0;i<saved.nr;i++) error(&r->cleanup_status,rr(saved.rf[i].bank,saved.rf[i].reg,saved.rf[i].before)); error(&r->cleanup_status,rr(0,59,saved.dpd_offset)); }
 else for(i=0;i<4;i++) filters[i]=next[i];
report:
 r->status=s?s:r->cleanup_status?MT7601U_CAL_RESTORE:0; r->valid=!r->status;
 for(i=0;i<4;i++) r->coefficients[i]=filters[i];
 /* Repeated channel applications during scans must not overwrite boot
  * evidence. Keep every measurement/error and first apply per mode/width. */
 if(!apply || r->status || !(bw_logged&(1u<<(width*2+mode)))) {
 if(apply && !r->status) bw_logged|=1u<<(width*2+mode);
 MT7601U_REPORT(MT_BW, "BW value=%08x status=%u cleanup=%u valid=%u n=%u polls=%u coef=%02x%02x%02x%02x\n",value,r->status,r->cleanup_status,r->valid,r->measurements,r->polls,filters[0],filters[1],filters[2],filters[3]);
 } return r->status;
}
static uint32_t dpd_metric_read(int32_t *metric) {
 uint8_t values[11],tail; unsigned i;
 TRY(mt7601u_bbp_write(4,16)); TRY(mt7601u_bbp_write(186,1));
 for(i=0;i<11;i++) { TRY(mt7601u_bbp_write(187,3+4*i)); TRY(mt7601u_bbp_read(188,&values[i])); trace(0x40000000u|((3+4*i)<<8)|values[i]); }
 TRY(mt7601u_bbp_write(187,43)); TRY(mt7601u_bbp_read(188,&tail)); trace(0x40010000u|tail); TRY(mt7601u_bbp_write(4,0));
 *metric=mt7601u_dpd_metric(values,tail); trace(0x80000000u|((uint32_t)*metric&65535)); return 0;
}
static uint32_t dpd_setup(int32_t temp,unsigned mode) {
 static const uint8_t force[]={1,0xe1,0x83,0x51,0x3c,0x24,0x12,0xa1,0x80,0xf1,0xa1,1}; unsigned i; uint8_t v;
 TRY(iw(3,0x60)); BWD_WRITE32(BYPASS,saved.bypass|0xc0080u);
 BWD_WRITE32(SETTING,(saved.setting&0x81fffe7fu)|mt7601u_dpd_setting(temp,mode)); BWD_WRITE32(AUX,0xc0020u);
 for(i=0;i<12;i++) TRY(mt7601u_rf_write(saved.rf[i].bank,saved.rf[i].reg,force[i]));
 TRY(mt7601u_bbp_write(23,16)); TRY(mt7601u_bbp_write(109,0)); TRY(mt7601u_bbp_write(110,0));
 BWD_WRITE32(MAC,8); TRY(iw(0x8c,0x40)); TRY(mt7601u_bbp_write(27,0)); TRY(mt7601u_bbp_write(65,0x39));
 /* Reference reads direct BBP140, not indexed BBP159. */
 for(i=0;i<100;i++) { TRY(mt7601u_bbp_read(140,&v)); if(!v) break; }
 if(i==100) return MT7601U_RF_BBP_POLL_TIMEOUT;
 TRY(table_write(0,2,10,0)); TRY(table_write(0,3,0x41,0));
 TRY(mt7601u_bbp_write(241,4)); TRY(mt7601u_bbp_write(242,0)); return 0;
}
static uint32_t dpd_search(unsigned mode,struct mt7601u_bw_dpd_result *r) {
 static const uint8_t gains[]={0x10,0x14,0x18,0x1c,0x20,0x30,0x34,0x38,0x3c,0x40,0x44,0x60,0x64,0x68,0x6c,0x70,0x74,0x78,0x7c};
 int gain=5; unsigned n=0,attempt,poll; uint8_t v,bw=saved.bbp[0]; int32_t metric=0; unsigned threshold=8;
 r->disabled[mode]=1;
 for(attempt=0;attempt<MT7601U_DPD_SEARCH_LIMIT;attempt++) {
  TRY(mt7601u_bbp_write(27,0)); TRY(mt7601u_bbp_write(66,gains[gain])); TRY(mt7601u_bbp_write(4,bw|16));
  TRY(mt7601u_bbp_write(244,0x11)); TRY(mt7601u_bbp_write(21,1)); TRY(mt7601u_bbp_write(21,0));
  TRY(table_write(0,1,0x80|mode,0)); TRY(tick(r));
  for(poll=0;poll<100;poll++) { TRY(mt7601u_bbp_read(188,&v)); r->polls++; if(!(v&128)) break; }
  if(poll==100) return MT7601U_RF_BBP_POLL_TIMEOUT;
  TRY(mt7601u_bbp_write(4,bw)); TRY(mt7601u_bbp_write(244,0)); TRY(mt7601u_bbp_write(21,1)); TRY(mt7601u_bbp_write(21,0));
  TRY(mt7601u_bbp_write(186,1)); TRY(mt7601u_bbp_write(187,255)); TRY(mt7601u_bbp_read(188,&v));
  r->measurements++; r->gain[mode]=gain; r->amplitude[mode]=v; trace((mode<<24)|(v<<16)|(n<<8)|(unsigned)gain);
  if(v<180) { if(++gain==19) break; }
  else if(v>=246) { if(--gain<0) break; }
  else {
   if(n%5==4) threshold+=10;
   if(n==5) break;
   TRY(dpd_metric_read(&metric)); r->metric[mode]=metric;
   if(metric>=-10 && metric<=(int32_t)threshold) { r->disabled[mode]=0; break; }
   n++;
  }
 }
 if(attempt==MT7601U_DPD_SEARCH_LIMIT) return 10; /* Oscillating original gain walk bounded explicitly. */
 return 0;
}
static uint32_t dpd_modes(int32_t temp,unsigned mode,unsigned disabled) {
 unsigned i; uint8_t v;
 TRY(mt7601u_bbp_write(4,saved.bbp[0]|16));
 for(i=4;i<8;i++) { TRY(mt7601u_bbp_write(186,i)); TRY(mt7601u_bbp_write(186,i)); TRY(mt7601u_bbp_write(189,15)); }
 TRY(mt7601u_bbp_write(186,2+mode)); TRY(mt7601u_bbp_write(186,2+mode)); TRY(mt7601u_bbp_read(189,&v));
 TRY(mt7601u_bbp_write(186,2+mode)); TRY(mt7601u_bbp_write(186,2+mode));
 return br(189,disabled?15:(v&192)|mt7601u_dpd_mode(temp,mode));
}
static uint32_t dpd_rescale(int32_t temp) {
 unsigned b,i; uint8_t v;
 for(b=2;b<4;b++) for(i=0;i<64;i++) {
  TRY(mt7601u_bbp_write(186,b)); TRY(mt7601u_bbp_write(187,i*4+3)); TRY(mt7601u_bbp_read(188,&v)); trace(0x20000000u|((b-2)<<16)|(i<<8)|v);
  TRY(mt7601u_bbp_write(188,mt7601u_dpd_scale(v,i)));
 }
 TRY(table_write(0,4,temp<-250?14:28,0)); return table_write(0,0,0xc0,0);
}
uint32_t mt7601u_dpd_calibrate(uint32_t temperature) {
 struct mt7601u_bw_dpd_result *r=&mt7601u_dpd_result; uint32_t s; unsigned mode=0;
 r->attempts++; r->valid=0; r->cleanup_status=r->measurements=r->polls=r->timer_last=0;
 r->temperature=signed32(temperature); r->metric[0]=r->metric[1]=0; r->disabled[0]=r->disabled[1]=1;
 BWD_WRITE32(MT7601U_IQ_TRACE_BASE,0); s=ready(); if(!s) s=capture(1); if(s) goto report;
 s=table_write(0,0,0,0); if(s) { cleanup(1,s,&r->cleanup_status); goto report; }
 for(mode=0;mode<2;mode++) {
  s=dpd_setup(r->temperature,mode); if(!s) s=dpd_search(mode,r); if(!s) s=dpd_modes(r->temperature,mode,r->disabled[mode]);
  if(!s && mode && !(r->disabled[0] && r->disabled[1])) s=dpd_rescale(r->temperature);
  cleanup(1,s,&r->cleanup_status); if(s || r->cleanup_status) break;
 }
report:
 r->status=s?s:r->cleanup_status?MT7601U_CAL_RESTORE:0; r->valid=!r->status;
 if(r->valid) { mt7601u_dpd_state.temperature=r->temperature; mt7601u_dpd_state.valid=1; mt7601u_dpd_state.disabled[0]=r->disabled[0]; mt7601u_dpd_state.disabled[1]=r->disabled[1]; }
 MT7601U_REPORT(MT_DPD, "DPD temp=%d status=%u cleanup=%u valid=%u n=%u polls=%u gain=%u,%u amp=%u,%u metric=%d,%d disabled=%u,%u timer=%08x\n",r->temperature,r->status,r->cleanup_status,r->valid,r->measurements,r->polls,r->gain[0],r->gain[1],r->amplitude[0],r->amplitude[1],r->metric[0],r->metric[1],r->disabled[0],r->disabled[1],r->timer_last); return r->status;
}
uint32_t mt7601u_tssi_kick(uint32_t value) {
 struct mt7601u_bw_dpd_result *r=&mt7601u_tssi_result; uint32_t s; uint8_t rf,b47,b58,v;
 value&=15; r->attempts++; r->valid=0; r->cleanup_status=0;
 s=ready(); if(s) goto report;
 s=mt7601u_rf_read(5,3,&rf); if(s) goto report;
 s=mt7601u_bbp_read(47,&b47); if(s) goto report;
 s=mt7601u_bbp_read(58,&b58); if(s) goto report;
 s=mt7601u_rf_write(5,3,(rf&224)|(value==1?17:8)); if(!s) s=mt7601u_bbp_write(47,(b47&159)|64);
 if(!s) s=mt7601u_bbp_write(58,0);
 if(!s) s=mt7601u_bbp_read(47,&v);
 if(!s) s=mt7601u_bbp_write(47,(v&128)|80);
 if(s) { error(&r->cleanup_status,rr(5,3,rf)); error(&r->cleanup_status,br(58,b58)); error(&r->cleanup_status,br(47,b47)); }
 /* This is a kick; completion and TSSI/temperature interpretation belong
  * to Linux. Do not poll away or reissue its measurement trigger. */
report:
 r->status=s?s:r->cleanup_status?MT7601U_CAL_RESTORE:0; r->valid=!r->status;
 if(r->status || !(tssi_logged&(1u<<value))) {
 if(!r->status) tssi_logged|=1u<<value;
 MT7601U_REPORT(MT_TSSI, "TSSI value=%u status=%u cleanup=%u valid=%u\n",value,r->status,r->cleanup_status,r->valid);
 } return r->status;
}
