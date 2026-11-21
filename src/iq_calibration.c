#include "mt7601u_iq_calibration.h"
#include "mt7601u_calibration.h"
#include "mt7601u_rf_bbp.h"
#include "mt7601u_mmio.h"
#include "mt7601u_runtime.h"
#ifndef IQ_READ32
#define IQ_READ32(a) MMIO32(a)
#define IQ_WRITE32(a,v) (MMIO32(a) = (v))
#endif
#define MAC 0x00411004u
#define AUX 0x004113b8u
#define BYPASS 0x00410504u
#define SETTING 0x0041050cu
#define SAMPLE 0x0041007cu
struct mt7601u_iq_result mt7601u_iq_loft, mt7601u_iq_tx, mt7601u_iq_rx;
static uint8_t retained_loft[6], retained_tx[2], retained_rx[2], retained_gain;
static const uint8_t tx_gain[19] = {0x24,0x25,0x26,0x27,0x28,0x2c,0x2d,0x2e,0x2f,0x30,0x31,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f};
static const uint8_t rx_gain[11] = {0x20,0x21,0x22,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f};
static const uint8_t bbp_regs[] = {4,21,23,31,241,242,244};
static const uint8_t idx_regs[] = {0,3,5,0x1e,0x28,0x29,0x35,0x37,0xaa,0xab,0xac,0xad,0xba};
struct rf_save { uint8_t bank, reg, temporary, before; };
struct context {
	struct rf_save rf[13];
	uint8_t rf_count, bbp[sizeof(bbp_regs)], idx[sizeof(idx_regs)];
	uint8_t selector, bank, loft[13][2];
	uint32_t mac, aux, bypass, setting;
};
static void error(uint32_t *first, uint32_t next) { if (!*first) *first = next; }
static void delay(unsigned groups)
{
	unsigned i,j;
	for (i=0;i<groups;i++) for (j=0;j<7u;j++) __asm__ volatile("nop");
}
static int32_t signed_bits(uint32_t value, unsigned bits)
{
	uint32_t mask = (1u<<bits)-1u, sign=1u<<(bits-1u);
	value &= mask;
	return value & sign ? -(int32_t)((mask+1u)-value) : (int32_t)value;
}
static int32_t signed32(uint32_t v)
{ return v <= 0x7fffffffu ? (int32_t)v : -1 - (int32_t)(~v); }
uint32_t mt7601u_iq_power(uint32_t sample)
{
	int32_t a=signed_bits(sample>>16,16), b=signed_bits(sample,16);
	return (uint32_t)(a*a)+(uint32_t)(b*b);
}
uint32_t mt7601u_iq_sqrt(uint32_t value)
{
	uint32_t estimate=600u; unsigned i;
	/* Zero makes the original divide by zero on its tenth iteration. It is
	 * an invalid RX measurement, so handle it explicitly before division. */
	if (!value) return 0;
	for(i=0;i<10u;i++) estimate=(value/estimate+estimate)>>1;
	return estimate;
}
void mt7601u_iq_decode_rx(struct mt7601u_iq_result *r)
{
	uint32_t a=r->raw[0], b=r->raw[1], vi,vq;
	int32_t cross=signed32(r->raw[4])/4096, ratio, delta, phase;
	if(a&0x800000u) a=(a&0xffffffu)|0xff000000u;
	if(b&0x800000u) b=(b&0xffffffu)|0xff000000u;
	r->mean_i=signed32(a)/4096; r->mean_q=signed32(b)/4096;
	vi=(r->raw[2]>>12)-(uint32_t)((uint32_t)r->mean_i*(uint32_t)r->mean_i);
	vq=(r->raw[3]>>12)-(uint32_t)((uint32_t)r->mean_q*(uint32_t)r->mean_q);
	r->rms_i=mt7601u_iq_sqrt(vi*100u); r->rms_q=mt7601u_iq_sqrt(vq*100u);
	r->gain=0; r->phase=0; r->measured=0;
	delta=(int32_t)r->rms_i-(int32_t)r->rms_q;
	if (r->rms_i-1000u>=401u || delta>=113 || delta<=-113 ||
	    (uint32_t)r->mean_i+32u>=65u || (uint32_t)r->mean_q+32u>=65u) return;
	ratio=(int32_t)(r->rms_q*1000u/r->rms_i);
	/* NDS32 mul/divsr are 32-bit. Avoid undefined signed C overflow. */
	phase=signed32(((uint32_t)cross-(uint32_t)((uint32_t)r->mean_q*(uint32_t)r->mean_i))*22920u);
	r->phase=phase/(int32_t)(r->rms_q*r->rms_i);
	r->gain=((ratio-1000)*256)/(ratio+1000);
	if(r->phase < -12 || r->phase > 12) r->phase=0;
	if(r->gain < -7 || r->gain > 7) r->gain=0;
	r->measured=1;
}
static uint32_t iw(unsigned reg, uint8_t value)
{
	uint32_t s=mt7601u_bbp_write(158,reg);
	return s?s:mt7601u_bbp_write(159,value);
}
static uint32_t ir(unsigned reg, uint8_t *value)
{
	uint32_t s=mt7601u_bbp_write(158,reg);
	return s?s:mt7601u_bbp_read(159,value);
}
static uint32_t br(unsigned reg, uint8_t value)
{
	uint8_t v=0; uint32_t s=mt7601u_bbp_write(reg,value), check=mt7601u_bbp_read(reg,&v);
	return s?s:check?check:v==value?0u:MT7601U_CAL_RESTORE;
}
static uint32_t restore_idx(unsigned reg, uint8_t value)
{
	uint8_t v=0; uint32_t s=iw(reg,value), check=ir(reg,&v);
	return s?s:check?check:v==value?0u:MT7601U_CAL_RESTORE;
}
static uint32_t lr(unsigned bank, uint8_t *a, uint8_t *b)
{
	uint32_t s=iw(0xb0,bank); if(s) return s;
	s=ir(0xb1,a); return s?s:ir(0xb2,b);
}
static uint32_t lw(unsigned bank, uint8_t a, uint8_t b, int verify)
{
	uint8_t x=0,y=0; uint32_t s=iw(0xb0,bank);
	if(!s) s=iw(0xb1,a);
	if(!s) s=iw(0xb2,b);
	if(verify && !s) { s=lr(bank,&x,&y); if(!s && (a!=x || b!=y)) s=MT7601U_CAL_RESTORE; }
	return s;
}
static uint32_t pulse(unsigned groups, uint8_t value)
{
	uint32_t s=mt7601u_bbp_write(21,value|1u); delay(groups);
	uint32_t t=mt7601u_bbp_write(21,value&0xfeu); return s?s:t;
}
static uint32_t capture(struct context *c, unsigned id)
{
	static const struct rf_save tx[]={{0,1,0x21,0},{0,2,0x10,0},{0,35,0,0},{0,42,0x1b,0},
		{4,0,0x81,0},{4,2,0x81,0},{4,34,0xee,0},{5,3,0x30,0},{5,4,0x30,0},
		{5,17,0x80,0},{5,18,0xd7,0},{5,19,0xa2,0},{5,20,0x20,0}};
	static const struct rf_save rx[]={{4,0,0x87,0},{4,19,0x24,0},{5,17,0x80,0},
		{5,18,0xc1,0},{5,19,0x60,0},{5,20,0,0},{5,3,0x3c,0},{5,4,0x3c,0}};
	uint32_t s; unsigned i; int bank_touched=0;
	c->rf_count=id==8?8:13;
	c->mac=IQ_READ32(MAC); c->aux=IQ_READ32(AUX);
	c->bypass=IQ_READ32(BYPASS); c->setting=IQ_READ32(SETTING);
	for(i=0;i<c->rf_count;i++) {
		c->rf[i]=id==8?rx[i]:tx[i];
		s=mt7601u_rf_read(c->rf[i].bank,c->rf[i].reg,&c->rf[i].before); if(s) return s;
	}
	for(i=0;i<sizeof(bbp_regs);i++) { s=mt7601u_bbp_read(bbp_regs[i],&c->bbp[i]); if(s) return s; }
	s=mt7601u_bbp_read(158,&c->selector); if(s) return s;
	/* Snapshot indirect state and restore the cursor even if a read failed. */
	for(i=0;i<sizeof(idx_regs);i++) { s=ir(idx_regs[i],&c->idx[i]); if(s) break; }
	if(!s) s=ir(0xb0,&c->bank);
	if(!s && id==4) { bank_touched=1; s=lr(0x80,&c->loft[12][0],&c->loft[12][1]); }
	if(!s && id==4) for(i=0;i<12u;i++) { bank_touched=1; s=lr((i/3)*4+i%3,&c->loft[i][0],&c->loft[i][1]); if(s) break; }
	/* Cursor writes have side effects only on selection, never on outputs. */
	if(bank_touched) {
		uint32_t restore=iw(0xb0,c->bank);
		if(restore) (void)iw(0xb0,c->bank);
		error(&s,restore);
	}
	error(&s,mt7601u_bbp_write(158,c->selector));
	if(s) return s;
	return c->idx[0] || (c->bbp[1]&1u) ? MT7601U_CAL_ACTIVE : 0u;
}
static void mmio_restore(struct context *c, unsigned id, uint32_t *err)
{
	IQ_WRITE32(BYPASS,0); IQ_WRITE32(SETTING,c->setting); IQ_WRITE32(BYPASS,c->bypass);
	if(IQ_READ32(SETTING)!=c->setting || IQ_READ32(BYPASS)!=c->bypass) error(err,MT7601U_CAL_RESTORE);
	/* MAC is restored last, after bounded RF/BBP cleanup. */
	if(id!=8) { IQ_WRITE32(AUX,c->aux); IQ_WRITE32(MAC,c->mac);
		if(IQ_READ32(AUX)!=c->aux || IQ_READ32(MAC)!=c->mac) error(err,MT7601U_CAL_RESTORE); }
}
static void rollback(struct context *c, unsigned id, uint32_t *err)
{
	unsigned i;
	for(i=0;i<sizeof(idx_regs);i++) error(err,restore_idx(idx_regs[i],c->idx[i]));
	if(id==4) {
		for(i=0;i<12u;i++) error(err,lw((i/3)*4+i%3,c->loft[i][0],c->loft[i][1],1));
		error(err,lw(0x80,c->loft[12][0],c->loft[12][1],1));
	}
	error(err,restore_idx(0xb0,c->bank));
	for(i=0;i<sizeof(bbp_regs);i++) error(err,br(bbp_regs[i],c->bbp[i]));
	error(err,br(158,c->selector));
}
static void cleanup(struct context *c, unsigned id, uint32_t failed, uint32_t *err)
{
	unsigned i; uint8_t v; uint32_t s;
	/* Stop the engine before restoring analog forcing, even after timeout. */
	error(err,restore_idx(0,0)); error(err,br(244,0));
	if(id==8) {
		IQ_WRITE32(SETTING,0x92018u); delay(1); IQ_WRITE32(SETTING,0x92008u); delay(1);
		IQ_WRITE32(SETTING,0x92000u); delay(1);
		s=mt7601u_bbp_read(21,&v); error(err,s); if(!s) error(err,pulse(10,v));
	} else error(err,pulse(1,0));
	if(id==4) error(err,br(31,8));
	else if(id==5) { error(err,restore_idx(0xb0,0)); error(err,br(31,c->bbp[3])); }
	IQ_WRITE32(BYPASS,0); IQ_WRITE32(SETTING,c->setting); IQ_WRITE32(BYPASS,c->bypass);
	for(i=0;i<c->rf_count;i++) {
		uint8_t readback=0; struct rf_save *r=&c->rf[i];
		s=mt7601u_rf_write(r->bank,r->reg,r->before); error(err,s);
		s=mt7601u_rf_read(r->bank,r->reg,&readback); error(err,s);
		/* On the original no-BW path RF 5:4 reads 0x44 before LOFT,
		 * but rewriting 0x44 reads 0x04. Bit 6 is not retained after
		 * analog forcing. Match the proven writable/readback fields. */
		if(!s && ((readback^r->before)&(r->bank==5 && r->reg==4?0xbfu:0xffu))) {
			mt7601u_log_printf("IQ restore RF %u:%u expected=%02x actual=%02x\n",r->bank,r->reg,r->before,readback);
			error(err,MT7601U_CAL_RESTORE);
		}
	}
	error(err,br(4,c->bbp[0])); if(id==8) error(err,br(23,c->bbp[2]));
	if(failed || *err) rollback(c,id,err);
	mmio_restore(c,id,err);
	/* A late MMIO failure invalidates compensation too. */
	if(!failed && *err) { if(id!=8) IQ_WRITE32(MAC,0); rollback(c,id,err); mmio_restore(c,id,err); }
}
#define TRY(x) do { uint32_t _s=(x); if(_s) return _s; } while(0)
static uint32_t setup(struct context *c, unsigned id)
{
	unsigned i; uint8_t v;
	if(id!=8) { IQ_WRITE32(AUX,0x10); IQ_WRITE32(MAC,0); }
	for(i=0;i<c->rf_count;i++) {
		if(id==5 && c->rf[i].bank==5 && (c->rf[i].reg==3 || c->rf[i].reg==4)) continue;
		TRY(mt7601u_rf_write(c->rf[i].bank,c->rf[i].reg,c->rf[i].temporary));
	}
	TRY(mt7601u_bbp_write(4,id==8?0:c->bbp[0]&0xe7u)); TRY(mt7601u_bbp_write(23,0));
	IQ_WRITE32(BYPASS,0); IQ_WRITE32(SETTING,id==8?0x92000u:0x52000u);
	IQ_WRITE32(BYPASS,id==8?0xdf838u:0xcf8b0u); delay(1);
	IQ_WRITE32(SETTING,id==8?0x92010u:0x5201eu); delay(1);
	IQ_WRITE32(SETTING,id==8?0x92030u:0x5203eu); delay(1);
	if(id==8) { IQ_WRITE32(SETTING,0x92038u); delay(1); }
	if(id==4) {
		TRY(iw(0xaa,0)); TRY(iw(0xab,10)); TRY(iw(0xac,63)); TRY(iw(0xad,63));
		TRY(mt7601u_bbp_write(244,0x40));
	} else {
		TRY(mt7601u_bbp_write(23,id==8?2:4)); TRY(mt7601u_bbp_write(241,0x14));
		TRY(mt7601u_bbp_write(242,0x80)); TRY(mt7601u_bbp_write(244,0x31));
	}
	if(id!=8) { TRY(mt7601u_bbp_write(31,c->bbp[3]|0x60u)); TRY(iw(0xb0,0x80)); }
	TRY(iw(5,id==5?4:0));
	if(id==5) { TRY(ir(3,&v)); TRY(iw(3,v|0x60u)); }
	if(id==8) TRY(iw(3,0x60));
	return 0;
}
static uint32_t trigger(struct mt7601u_iq_result *r, uint8_t command, unsigned limit)
{
	uint8_t v=command; unsigned i;
	TRY(iw(0,command));
	for(i=0;i<limit;i++) { delay(10); TRY(mt7601u_bbp_read(159,&v)); r->polls++; if(v!=command) return 0; }
	return MT7601U_RF_BBP_POLL_TIMEOUT;
}
static uint32_t power(struct mt7601u_iq_result *r, unsigned id, uint32_t *out)
{
	uint32_t raw, n;
	TRY(pulse(id==4?1:10,0)); TRY(trigger(r,0x9b,id==4?101:1000));
	TRY(iw(0xba,id==4?10:20)); raw=IQ_READ32(SAMPLE); *out=mt7601u_iq_power(raw);
	n=r->measurements++;
	if(n>=MT7601U_IQ_TRACE_LIMIT) return MT7601U_RF_BBP_INVALID;
	IQ_WRITE32(MT7601U_IQ_TRACE_BASE+4+n*8,raw);
	IQ_WRITE32(MT7601U_IQ_TRACE_BASE+8+n*8,*out);
	IQ_WRITE32(MT7601U_IQ_TRACE_BASE,n+1); r->power=*out;
	return 0;
}
static uint32_t loft_measure(struct mt7601u_iq_result *r, unsigned axis, uint8_t code, uint32_t *p)
{ TRY(iw(0xb0,0x80)); TRY(iw(0xb1+axis,code)); return power(r,4,p); }
static uint32_t loft(struct mt7601u_iq_result *r)
{
	static const uint8_t bbp23[3]={22,20,18};
	unsigned bank, axis, step, i; uint32_t minus,plus,best; uint8_t v[2],lo,hi;
	r->gain_index=18;
	for(bank=0;bank<3;bank++) {
		IQ_WRITE32(SETTING,(bank<<25)|0x1203eu); TRY(mt7601u_bbp_write(23,bbp23[bank]));
		TRY(iw(0xb0,0x80)); TRY(iw(0xb2,0));
		if(!bank) {
			/* The reference tests 18..2 and exits at 0 without a new sample. */
			for(i=18;i>0;i-=2) {
				r->gain_index=i; TRY(mt7601u_rf_write(5,3,tx_gain[i])); TRY(mt7601u_rf_write(5,4,tx_gain[i]));
				TRY(loft_measure(r,0,0,&minus)); TRY(loft_measure(r,0,33,&plus));
				if(minus<49000000u && plus<49000000u) break;
			}
			r->gain_index=i;
		}
		TRY(iw(0xb1,0)); v[0]=v[1]=0; best=0;
		for(step=32;step;step>>=1) for(axis=0;axis<2;axis++) {
			lo=v[axis]; minus=best;
			if(v[axis]!=32) { lo=(v[axis]-step)&63u; TRY(loft_measure(r,axis,lo,&minus)); }
			hi=(v[axis]+(step==32?0:step))&63u; TRY(loft_measure(r,axis,hi,&plus));
			if(step==32 || minus<best || plus<=best) { if(minus<plus) { v[axis]=lo; best=minus; } else { v[axis]=hi; best=plus; } }
			TRY(iw(0xb0,0)); TRY(iw(0xb1+axis,v[axis]));
		}
		r->coefficients[bank*2]=v[0]; r->coefficients[bank*2+1]=v[1];
	}
	r->measured=1; return 0;
}
static uint32_t tx_measure(struct mt7601u_iq_result *r, unsigned axis, uint8_t code, uint32_t *p)
{ TRY(iw(0x28+axis,code)); return power(r,5,p); }
static uint32_t txiq(struct mt7601u_iq_result *r)
{
	uint8_t v[2]={0,0}, lo,hi, mask,sentinel; unsigned i, count,step,axis;
	int g,p,sg,sp; uint32_t minus,plus,best=0;
	TRY(mt7601u_rf_write(5,3,0x3c)); TRY(mt7601u_rf_write(5,4,0x3c)); TRY(iw(0x28,0));
	i=retained_gain>18?18:retained_gain;
	for(;i<19;i+=2) {
		TRY(mt7601u_rf_write(5,3,tx_gain[i])); TRY(mt7601u_rf_write(5,4,tx_gain[i]));
		TRY(tx_measure(r,1,33,&minus)); TRY(tx_measure(r,1,0,&plus));
		if(minus>6250000u || plus>6250000u) break;
	}
	r->gain_index=i;
	for(count=5,step=32;count;count--,step>>=1) for(axis=0;axis<2;axis++) {
		int first=count==(axis?5u:3u);
		if(!axis && count>=4) continue;
		mask=axis?63:15; sentinel=axis?32:8; lo=v[axis]; minus=best;
		if((v[axis]&mask)!=sentinel) { lo=(v[axis]-step)&mask; TRY(tx_measure(r,axis,lo,&minus)); }
		hi=(v[axis]+(first?0:step))&mask; TRY(tx_measure(r,axis,hi,&plus));
		if(first || minus<best || plus<=best) { if(minus<plus) { v[axis]=lo; best=minus; } else { v[axis]=hi; best=plus; } }
		TRY(iw(0x28+axis,v[axis]));
	}
	TRY(mt7601u_bbp_write(23,7));
	g=signed_bits(v[0],4); p=signed_bits(v[1],6);
	if(g>3) g=4;
	sg=g<-7?-8:g-1;
	if(p>27) p=28;
	sp=p<-31?-32:p-1;
	for(g=sg;g<sg+4;g++) for(p=sp;p<sp+4;p++) {
		TRY(iw(0x28,g&15)); TRY(iw(0x29,p&63)); TRY(power(r,5,&plus));
		if((g==sg && p==sp) || plus<best) { best=plus; v[0]=g&15; v[1]=p&63; }
	}
	r->coefficients[0]=v[0]; r->coefficients[1]=v[1]; r->power=best;
	r->gain=signed_bits(v[0],4); r->phase=signed_bits(v[1],6); r->measured=1;
	return 0;
}
static uint32_t rxiq(struct mt7601u_iq_result *r)
{
	unsigned gain=0,stat,byte; uint8_t v; uint32_t variance,step;
	while(gain<11u) {
		r->gain_index=gain; TRY(mt7601u_rf_write(5,3,rx_gain[gain])); TRY(mt7601u_rf_write(5,4,rx_gain[gain]));
		TRY(trigger(r,0x93,1000)); r->measurements++;
		for(stat=0;stat<5u;stat++) {
			TRY(iw(0x1e,stat)); r->raw[stat]=0;
			for(byte=0;byte<4u;byte++) { TRY(ir(0x22-byte,&v)); r->raw[stat]=(r->raw[stat]<<8)|v; }
		}
		for(stat=0;stat<5u;stat++) IQ_WRITE32(MT7601U_IQ_TRACE_BASE+4+stat*4,r->raw[stat]);
		IQ_WRITE32(MT7601U_IQ_TRACE_BASE,r->measurements);
		mt7601u_iq_decode_rx(r); variance=(r->raw[2]>>12)-(uint32_t)((uint32_t)r->mean_i*(uint32_t)r->mean_i);
		if(variance>9999u) break;
		step=variance<101?9:variance<159?8:variance<252?7:variance<399?6:variance<631?5:
			variance<1001?4:variance<1585?3:variance<2512?2:1;
		gain+=step;
	}
	r->coefficients[0]=(uint32_t)r->gain&63u; r->coefficients[1]=(uint32_t)r->phase&63u;
	return 0;
}
static uint32_t apply(unsigned id, const uint8_t *v)
{
	unsigned i; uint8_t old;
	if(id==4) for(i=0;i<12u;i++) { TRY(lw((i/3)*4+i%3,v[(i%3)*2],v[(i%3)*2+1],1)); }
	else {
		if(id==8) { TRY(ir(3,&old)); TRY(restore_idx(3,old|7u)); }
		TRY(restore_idx(id==5?0x28:0x37,v[0])); TRY(restore_idx(id==5?0x29:0x35,v[1]));
		TRY(restore_idx(0,0));
		if(id==5) { TRY(br(244,0)); TRY(pulse(1,0)); }
	}
	return 0;
}
uint32_t mt7601u_iq_calibrate(unsigned id, uint32_t value)
{
	struct mt7601u_iq_result *r=id==4?&mt7601u_iq_loft:id==5?&mt7601u_iq_tx:id==8?&mt7601u_iq_rx:0;
	struct context c; uint8_t version,*retained; uint32_t s; unsigned i,attempts;
	if(!r) return MT7601U_RF_BBP_INVALID;
	attempts=r->attempts+1u;
	for(i=0;i<sizeof(*r);i++) ((volatile uint8_t *)r)[i]=0;
	r->attempts=attempts;
	retained=id==4?retained_loft:id==5?retained_tx:retained_rx;
	if((IQ_READ32(0x00410080u)&3u)!=3u || (IQ_READ32(MAC)&3u)) s=MT7601U_CAL_NOT_READY;
	else { s=mt7601u_bbp_read(0,&version); if(!s && (!version || version==255u)) s=MT7601U_CAL_NOT_READY; }
	if(s) { r->status=s; goto report; }
	s=capture(&c,id); if(s) { r->status=s; goto report; }
	if(value&1u) {
		for(i=0;i<(id==4?6u:2u);i++) r->coefficients[i]=retained[i];
		s=apply(id,r->coefficients);
		if(s) { rollback(&c,id,&r->cleanup_status); mmio_restore(&c,id,&r->cleanup_status); }
	} else {
		IQ_WRITE32(MT7601U_IQ_TRACE_BASE,0);
		s=setup(&c,id);
		if(!s) s=id==4?loft(r):id==5?txiq(r):rxiq(r);
		if(!s) s=apply(id,r->coefficients);
		cleanup(&c,id,s,&r->cleanup_status);
	}
	r->status=s?s:r->cleanup_status;
	if(!r->status) {
		for(i=0;i<(id==4?6u:2u);i++) retained[i]=r->coefficients[i];
		if(id!=8 && !(value&1u)) retained_gain=r->gain_index;
		r->valid=1;
	}
report:
	MT7601U_REPORT(MT_IQ, "IQ id=%u mode=%u status=%u cleanup=%u valid=%u measured=%u n=%u polls=%u gi=%u coef=%02x%02x%02x%02x%02x%02x p=%u\n",
		id,value&1u,r->status,r->cleanup_status,r->valid,r->measured,r->measurements,r->polls,r->gain_index,
		r->coefficients[0],r->coefficients[1],r->coefficients[2],r->coefficients[3],r->coefficients[4],r->coefficients[5],r->power);
	if(id==8 && !(value&1u)) MT7601U_REPORT(MT_IQ_RX, "IQ RX raw=%08x,%08x,%08x,%08x,%08x mean=%d,%d rms=%u,%u correction=%d,%d\n",
		r->raw[0],r->raw[1],r->raw[2],r->raw[3],r->raw[4],r->mean_i,r->mean_q,r->rms_i,r->rms_q,r->gain,r->phase);
	return r->status;
}
