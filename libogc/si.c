#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "asm.h"
#include "processor.h"
#include "system.h"
#include "ogcsys.h"
#include "video.h"
#include "irq.h"
#include "si.h"

//#define _SI_DEBUG

#define _SHIFTL(v, s, w)	\
    ((u32) (((u32)(v) & ((0x01 << (w)) - 1)) << (s)))
#define _SHIFTR(v, s, w)	\
    ((u32)(((u32)(v) >> (s)) & ((0x01 << (w)) - 1)))

#define SICHAN_0					0x80000000
#define SICHAN_1					0x40000000
#define SICHAN_2					0x20000000
#define SICHAN_3					0x10000000
#define SICHAN_BIT(chn)				(SICHAN_0>>chn)

//
// CMD_TYPE_AND_STATUS response data
//
#define SI_TYPE_MASK            0x18000000u
#define SI_TYPE_N64             0x00000000u
#define SI_TYPE_DOLPHIN         0x08000000u
#define SI_TYPE_GC              SI_TYPE_DOLPHIN

// GameCube specific
#define SI_GC_WIRELESS          0x80000000u
#define SI_GC_NOMOTOR           0x20000000u // no rumble motor
#define SI_GC_STANDARD          0x01000000u // dolphin standard controller

// WaveBird specific
#define SI_WIRELESS_RECEIVED    0x40000000u // 0: no wireless unit
#define SI_WIRELESS_IR          0x04000000u // 0: IR  1: RF
#define SI_WIRELESS_STATE       0x02000000u // 0: variable  1: fixed
#define SI_WIRELESS_ORIGIN      0x00200000u // 0: invalid  1: valid
#define SI_WIRELESS_FIX_ID      0x00100000u // 0: not fixed  1: fixed
#define SI_WIRELESS_TYPE        0x000f0000u
#define SI_WIRELESS_LITE_MASK   0x000c0000u // 0: normal 1: lite controller
#define SI_WIRELESS_LITE        0x00040000u // 0: normal 1: lite controller
#define SI_WIRELESS_CONT_MASK   0x00080000u // 0: non-controller 1: non-controller
#define SI_WIRELESS_CONT        0x00000000u
#define SI_WIRELESS_ID          0x00c0ff00u
#define SI_WIRELESS_TYPE_ID     (SI_WIRELESS_TYPE | SI_WIRELESS_ID)

#define SI_N64_CONTROLLER       (SI_TYPE_N64 | 0x05000000)
#define SI_N64_MIC              (SI_TYPE_N64 | 0x00010000)
#define SI_N64_KEYBOARD         (SI_TYPE_N64 | 0x00020000)
#define SI_N64_MOUSE            (SI_TYPE_N64 | 0x02000000)
#define SI_GBA                  (SI_TYPE_N64 | 0x00040000)
#define SI_GC_CONTROLLER        (SI_TYPE_GC | SI_GC_STANDARD)
#define SI_GC_RECEIVER          (SI_TYPE_GC | SI_GC_WIRELESS)
#define SI_GC_WAVEBIRD          (SI_TYPE_GC | SI_GC_WIRELESS | SI_GC_STANDARD | SI_WIRELESS_STATE | SI_WIRELESS_FIX_ID)
#define SI_GC_KEYBOARD          (SI_TYPE_GC | 0x00200000)
#define SI_GC_STEERING          (SI_TYPE_GC | 0x00000000)

#define SISR_ERRORMASK(chn)			(0x0f000000>>(chn<<3))
#define SIPOLL_ENABLE(chn)			(0x80000000>>(chn+24))

#define SICOMCSR_TCINT				(1<<31)
#define SICOMCSR_TCINT_ENABLE		(1<<30)
#define SICOMCSR_COMERR				(1<<29)
#define SICOMCSR_RDSTINT			(1<<28)
#define SICOMCSR_RDSTINT_ENABLE		(1<<27)

#define SISR_UNDERRUN				0x0001
#define SISR_OVERRUN				0x0002
#define SISR_COLLISION				0x0004
#define SISR_NORESPONSE				0x0008
#define SISR_WRST					0x0010
#define SISR_RDST					0x0020

typedef union _sicomcsr {
	u32 val;
	struct {
		u32 tcint		: 1;
		u32 tcintmsk	: 1;
		u32 comerr		: 1;
		u32 rdstint		: 1;
		u32 rdstintmsk	: 1;
		u32 pad2		: 4;
		u32 outlen		: 7;
		u32 pad1		: 1;
		u32 inlen		: 7;
		u32 pad0		: 5;
		u32 channel		: 2;
		u32 tstart		: 1;
	} csrmap;
} sicomcsr;

static struct _sipacket {
	s32 chan;
	void *out;
	u32 out_bytes;
	void *in;
	u32 in_bytes;
	SICallback callback;
	u32 fire;
} sipacket[4];

static struct _sicntrl {
	s32 chan;
	u32 poll;
	u32 in_bytes;
	void *in;
	SICallback callback;
} sicntrl = {
	-1,
	0,
	0,
	NULL,
	NULL
};

static struct _xy {
	u16 line;
	u8 cnt;
} xy[2][12] = {
	{
		{0x00F6,0x02},{0x000F,0x12},{0x001E,0x09},{0x002C,0x06},
		{0x0034,0x05},{0x0041,0x04},{0x0057,0x03},{0x0057,0x03},
		{0x0057,0x03},{0x0083,0x02},{0x0083,0x02},{0x0083,0x02}
	},

	{
		{0x0128,0x02},{0x000F,0x15},{0x001D,0x0B},{0x002D,0x07},
		{0x0034,0x06},{0x003F,0x05},{0x004E,0x04},{0x0068,0x03},
		{0x0068,0x03},{0x0068,0x03},{0x0068,0x03},{0x009C,0x02}
	}
};

u32 __PADFixBits = 0;

static u32 sampling_rate = 0;
static u32 cmdtypeandstatus$47 = 0;
static u32 cmdtypeandstatus$223 = 0;
static u32 cmdfixdevice[4] = {0,0,0,0};
static u32 si_type[4] = {8,8,8,8};
static u32 inputBufferVCount[4] = {0,0,0,0};
static u32 inputBufferValid[4] = {0,0,0,0};
static u32 inputBuffer[4][2] = {{0,0},{0,0},{0,0},{0,0}};
static RDSTHandler rdstHandlers[4] = {NULL,NULL,NULL,NULL};
static s64 typeTime[4] = {0,0,0,0};
static s64 xferTime[4] = {0,0,0,0};
static SICallback typeCallback[4][4] = {{NULL,NULL,NULL,NULL},
										{NULL,NULL,NULL,NULL},
										{NULL,NULL,NULL,NULL},
										{NULL,NULL,NULL,NULL}};
static sysalarm si_alarm[4];

static vu32* const _siReg = (u32*)0xCC006400;
static vu16* const _viReg = (u16*)0xCC002000;

static u32 __si_transfer(s32 chan,void *out,u32 out_len,void *in,u32 in_len,SICallback cb);

extern void __UnmaskIrq(u32);
extern void __MaskIrq(u32);
extern long long gettime();
extern u32 diff_usec(long long start,long long end);

static __inline__ struct _xy* __si_getxy()
{
	switch(VIDEO_GetCurrentTvMode()) {
		case VI_NTSC:
		case VI_MPAL:
		case VI_EURGB60:
			return xy[0];
			break;
		case VI_PAL:
			return xy[1];
			break;
	}
	return NULL;
}

static __inline__ void __si_cleartcinterrupt()
{
	_siReg[13] = (_siReg[13]|SICOMCSR_TCINT)&SICOMCSR_TCINT;
}

static void __si_alarmhandler(sysalarm *alarm)
{
	u32 chn;
#ifdef _SI_DEBUG
	printf("__si_alarmhandler(%p)\n",alarm);
#endif
	chn = 0;
	while(chn<4) {
		if((&si_alarm[chn])==alarm) break;
		chn++;
	}
	if(chn==4) return;

	if(sipacket[chn].chan!=-1) {
		if(__si_transfer(sipacket[chn].chan,sipacket[chn].out,sipacket[chn].out_bytes,sipacket[chn].in,sipacket[chn].in_bytes,sipacket[chn].callback)) sipacket[chn].chan = -1;
	}
}

static u32 __si_completetransfer()
{
	u32 val,sisr,cnt,i;
	u32 *in;

#ifdef _SI_DEBUG
	printf("__si_completetransfer(csr = %08x,sr = %08x,chan = %d)\n",_siReg[13],_siReg[14],sicntrl.chan);
#endif
	sisr = _siReg[14];
	__si_cleartcinterrupt();
	
	if(sicntrl.chan==-1) return sisr;

	xferTime[sicntrl.chan] = gettime();
	
	in = (u32*)sicntrl.in;
	for(cnt=0;cnt<(sicntrl.in_bytes/4);cnt++) in[cnt] = _siReg[32+cnt];
	if(sicntrl.in_bytes&0x03) {
		val = _siReg[32+cnt];
		for(i=0;i<(sicntrl.in_bytes&0x03);i++) ((u8*)in)[(cnt*4)+i] = (val>>((3-i)*8))&0xff;
	}
#ifdef _SI_DEBUG
	printf("__si_completetransfer(csr = %08x)\n",_siReg[13]);
#endif
	if(_siReg[13]&SICOMCSR_COMERR) {
		sisr = (sisr>>((3-sicntrl.chan)*8))&0x0f;
		if(sisr&SISR_NORESPONSE && !(si_type[sicntrl.chan]&0x80)) si_type[sicntrl.chan] = 8;
		if(!sisr) sisr = 4;
	} else {
		typeTime[sicntrl.chan] = gettime();
		sisr = 0;
	}
	
	sicntrl.chan = -1;
	return sisr;	
}

static u32 __si_transfer(s32 chan,void *out,u32 out_len,void *in,u32 in_len,SICallback cb)
{
	u32 level,cnt;
	sicomcsr csr;
#ifdef _SI_DEBUG
	printf("__si_transfer(%d,%p,%d,%p,%d,%p)\n",chan,out,out_len,in,in_len,cb);
#endif
	_CPU_ISR_Disable(level);
	if(sicntrl.chan!=-1) {
		_CPU_ISR_Restore(level);
		return 0;
	}
#ifdef _SI_DEBUG
	printf("__si_transfer(out = %08x,csr = %08x,sr = %08x)\n",*(u32*)out,_siReg[13],_siReg[14]);
#endif
	_siReg[14] &= SISR_ERRORMASK(chan);

	sicntrl.chan = chan;
	sicntrl.callback = cb;
	sicntrl.in_bytes = in_len;
	sicntrl.in = in;
#ifdef _SI_DEBUG
	printf("__si_transfer(csr = %08x,sr = %08x)\n",_siReg[13],_siReg[14]);
#endif
	for(cnt=0;cnt<((out_len+3)/4);cnt++)  _siReg[32+cnt] = ((u32*)out)[cnt];

	csr.val = _siReg[13];
	csr.csrmap.tcint = 1;
	csr.csrmap.tcintmsk = 0;
	if(cb) csr.csrmap.tcintmsk = 1;

	if(out_len==128) out_len = 0;
	csr.csrmap.outlen = out_len&0x7f;
	
	if(in_len==128) in_len = 0;
	csr.csrmap.inlen = in_len&0x7f;

	csr.csrmap.channel = chan&0x03;
	csr.csrmap.tstart = 1;
#ifdef _SI_DEBUG
	printf("__si_transfer(csr = %08x)\n",csr.val);
#endif
	_siReg[13] = csr.val;
	_CPU_ISR_Restore(level);

	return 1;
}

static void __si_calltypandstatuscallback(s32 chan,u32 type)
{
	u32 typ;
	SICallback cb = NULL;
#ifdef _SI_DEBUG
	printf("__si_calltypandstatuscallback(%d,%08x)\n",chan,type);
#endif
	typ = 0;
	while(typ<4) {
		cb = typeCallback[chan][typ];
		if(cb) {
			typeCallback[chan][typ] = NULL;
			cb(chan,type);
		}
		typ++;
	}
}

static void __si_gettypecallback(s32 chan,u32 type)
{
	u32 sipad_en,id;

	si_type[chan] = (si_type[chan]&~0x80)|type;
	typeTime[chan] = gettime();
#ifdef _SI_DEBUG
	printf("__si_gettypecallback(%d,%08x,%08x)\n",chan,type,si_type[chan]);
#endif
	sipad_en = __PADFixBits&SICHAN_BIT(chan);
	__PADFixBits &= ~SICHAN_BIT(chan);
	
	if(type&0x0f || ((si_type[chan]&SI_TYPE_MASK)-SI_TYPE_GC)
		|| !(si_type[chan]&SI_GC_WIRELESS) || si_type[chan]&SI_WIRELESS_IR) {
		SYS_SetWirelessID(chan,0);
		__si_calltypandstatuscallback(chan,si_type[chan]);
		return;
	}

	id = ((SYS_GetWirelessID(chan)<<8)&0x00ffff00);
#ifdef _SI_DEBUG
	printf("__si_gettypecallback(id = %08x)\n",id);
#endif
	if(sipad_en && id&SI_WIRELESS_FIX_ID) {
		cmdfixdevice[chan] = 0x4e100000|(id&0x00CFFF00);
		si_type[chan] = 128;
		SI_Transfer(chan,&cmdfixdevice[chan],3,&si_type[chan],3,__si_gettypecallback,0);
		return;
	}

	if(si_type[chan]&SI_WIRELESS_FIX_ID) {
		if((id&0x00CFFF00)==(si_type[chan]&0x00CFFF00)) goto exit;
		if(!(id&SI_WIRELESS_FIX_ID)) {
			id = SI_WIRELESS_FIX_ID|(si_type[chan]&0x00CFFF00);
			SYS_SetWirelessID(chan,_SHIFTR(id,8,16));
		}
		cmdfixdevice[chan] = 0x4e000000|id;
		si_type[chan] = 128;
		SI_Transfer(chan,&cmdfixdevice[chan],3,&si_type[chan],3,__si_gettypecallback,0);
		return;
	}
	
	if(si_type[chan]&SI_WIRELESS_RECEIVED) {
		id = 0x00100000|(si_type[chan]&0x00CFFF00);
		SYS_SetWirelessID(chan,_SHIFTR(id,8,16));

		cmdfixdevice[chan] = 0x4e000000|id;
		si_type[chan] = 128;
		SI_Transfer(chan,&cmdfixdevice[chan],3,&si_type[chan],3,__si_gettypecallback,0);
		return;
	}
	SYS_SetWirelessID(chan,0);

exit:
	__si_calltypandstatuscallback(chan,si_type[chan]);
}

static void __si_transfernext(u32 chan)
{
	u32 cnt;

#ifdef _SI_DEBUG
	printf("__si_transfernext(%d)\n",chan);
#endif
	cnt = 0;
	while(cnt<4) {
		chan++;
		chan %= 4;
#ifdef _SI_DEBUG
		printf("__si_transfernext(chan = %d,sipacket.chan = %d)\n",chan,sipacket[chan].chan);
#endif
		if(sipacket[chan].chan!=-1) {
			if(!__si_transfer(sipacket[chan].chan,sipacket[chan].out,sipacket[chan].out_bytes,sipacket[chan].in,sipacket[chan].in_bytes,sipacket[chan].callback)) break;
			SYS_CancelAlarm(&si_alarm[chan]);
			sipacket[chan].chan = -1;
		}
		cnt++;
	}
}

static void __si_interrupthandler(u32 irq,void *ctx)
{
	SICallback cb;
	u32 chn,curr_line,line,ret;
	sicomcsr csr;

	csr.val = _siReg[13];
#ifdef _SI_DEBUG
	printf("__si_interrupthandler(csr = %08x)\n",csr.val);
#endif
	if(csr.csrmap.tcintmsk && csr.csrmap.tcint) {
		chn = sicntrl.chan;
		cb = sicntrl.callback;
		sicntrl.callback = NULL;

		ret = __si_completetransfer();
		__si_transfernext(chn);

		if(cb) cb(chn,ret);
		
		_siReg[14] &= SISR_ERRORMASK(chn);

		if(si_type[chn]==128 && !SI_IsChanBusy(chn)) SI_Transfer(chn,&cmdtypeandstatus$47,1,&si_type[chn],3,__si_gettypecallback,65);
	}

	if(csr.csrmap.rdstintmsk && csr.csrmap.rdstint) {
		curr_line = VIDEO_GetCurrentLine();
		curr_line++;
		line = _SHIFTR(sicntrl.poll,16,10);
		
		chn = 0;
		while(chn<4) {
			if(SI_GetResponseRaw(chn)) inputBufferVCount[chn] = curr_line;	
			chn++;
		}

		chn = 0;
		while(chn<4) {
			if(sicntrl.poll&SIPOLL_ENABLE(chn)) {
				if(!inputBufferVCount[chn] || ((line>>1)+inputBufferVCount[chn])<curr_line) return;
			}
			chn++;
		}

		chn = 0;
		while(chn<4) inputBufferVCount[chn++] = 0;

		chn = 0;
		while(chn<4) {
			if(rdstHandlers[chn]) rdstHandlers[chn](irq,ctx);
			chn++;
		}
	}
}

u32 SI_Busy()
{
	return (sicntrl.chan==-1)?0:1;	
}

u32 SI_IsChanBusy(s32 chan)
{
	u32 ret = 0;

	if(sipacket[chan].chan!=-1 || sicntrl.chan==chan) ret = 1;
	
	return ret;
}

void SI_SetXY(u16 line,u8 cnt)
{
	u32 level;
#ifdef _SI_DEBUG
	printf("SI_SetXY(%d,%d)\n",line,cnt);
#endif
	_CPU_ISR_Disable(level);
	sicntrl.poll = (sicntrl.poll&~0x3ffff00)|_SHIFTL(line,16,10)|_SHIFTL(cnt,8,8);
	_siReg[12] = sicntrl.poll;
	_CPU_ISR_Restore(level);
}

void SI_EnablePolling(u32 poll)
{
	u32 level,mask;
#ifdef _SI_DEBUG
	printf("SI_EnablePolling(%08x)\n",poll);
#endif
	_CPU_ISR_Disable(level);
	poll >>= 24;
	mask = (poll>>4)&0x0f;
	sicntrl.poll &= ~mask;
	
	poll &= (0x03ffffff|mask);
	
	sicntrl.poll |= (poll&~0x03ffff00);
	SI_TransferCommands();
#ifdef _SI_DEBUG
	printf("SI_EnablePolling(%08x)\n",sicntrl.poll);
#endif
	_siReg[12] = sicntrl.poll;
	_CPU_ISR_Restore(level);
}

void SI_DisablePolling(u32 poll)
{
	u32 level,mask;
#ifdef _SI_DEBUG
	printf("SI_DisablePolling(%08x)\n",poll);
#endif
	_CPU_ISR_Disable(level);
	mask = (poll>>24)&0xf0;
	sicntrl.poll &= ~mask;
	_siReg[12] = sicntrl.poll;
	_CPU_ISR_Restore(level);
}

void SI_SetSamplingRate(u32 samplingrate)
{
	u32 div,level;
	struct _xy *xy = NULL;
	
	if(samplingrate>11) samplingrate = 11;

	_CPU_ISR_Disable(level);
	sampling_rate = samplingrate;
	xy = __si_getxy();
	
	div = 1;
	if(_viReg[54]&0x0001) div = 2;

	SI_SetXY(div*xy[samplingrate].line,xy[samplingrate].cnt);
	_CPU_ISR_Restore(level);
}

void SI_RefreshSamplingRate()
{
	SI_SetSamplingRate(sampling_rate);
}

u32 SI_GetStatus(s32 chan)
{
	u32 level,sisr;

	_CPU_ISR_Disable(level);
	sisr = (_siReg[14]>>((3-chan)<<3));
	if(sisr&SISR_NORESPONSE && !(si_type[chan]&0x80)) si_type[chan] = 8;
	_CPU_ISR_Restore(level);
	return sisr;
}

u32 SI_GetResponseRaw(s32 chan)
{
	u32 status,ret;
#ifdef _SI_DEBUG
	printf("SI_GetResponseRaw(%d)\n",chan);
#endif
	ret = 0;
	status = SI_GetStatus(chan);
	if(status&0x0020) {
		inputBuffer[chan][0] = _siReg[(chan*3)+1];
		inputBuffer[chan][1] = _siReg[(chan*3)+2];
		inputBufferValid[chan] = 1;
		ret = 1;		
	}
	return ret;
}

u32 SI_GetResponse(s32 chan,void *buf)
{
	u32 level,valid;
	_CPU_ISR_Disable(level);
	SI_GetResponseRaw(chan);
	valid = inputBufferValid[chan];
	inputBufferValid[chan] = 0;
#ifdef _SI_DEBUG
	printf("SI_GetResponse(%d,%p,%d)\n",chan,buf,valid);
#endif
	if(valid) {
		((u32*)buf)[0] = inputBuffer[chan][0];
		((u32*)buf)[1] = inputBuffer[chan][1];
	}
	_CPU_ISR_Restore(level);
	return valid;
}

void SI_SetCommand(s32 chan,u32 cmd)
{
	_siReg[chan*3] = cmd;
}

u32 SI_GetCommand(s32 chan)
{
	return (_siReg[chan*3]);
}
u32 SI_Transfer(s32 chan,void *out,u32 out_len,void *in,u32 in_len,SICallback cb,u32 us_delay)
{
	u32 ret = 0;
	u32 level;
	struct timespec tb;
#ifdef _SI_DEBUG
	printf("SI_Transfer(%d,%p,%d,%p,%d,%p,%d)\n",chan,out,out_len,in,in_len,cb,us_delay);
#endif
	_CPU_ISR_Disable(level);
	if(sipacket[chan].chan==-1 && sicntrl.chan!=chan) {
		ret = 1;
		if(us_delay) {
			tb.tv_sec = 0;
			tb.tv_nsec = us_delay*TB_NSPERUS;
			SYS_SetAlarm(&si_alarm[chan],&tb,__si_alarmhandler);
		} else if(__si_transfer(chan,out,out_len,in,in_len,cb)) {
			_CPU_ISR_Restore(level);
			return ret;
		}
		sipacket[chan].chan = chan;
		sipacket[chan].out = out;
		sipacket[chan].out_bytes = out_len;
		sipacket[chan].in = in;
		sipacket[chan].in_bytes = in_len;
		sipacket[chan].callback = cb;
		sipacket[chan].fire = us_delay;
#ifdef _SI_DEBUG
		printf("SI_Transfer(%d,%p,%d,%p,%d,%p,%d)\n",sipacket[chan].chan,sipacket[chan].out,sipacket[chan].out_bytes,sipacket[chan].in,sipacket[chan].in_bytes,sipacket[chan].callback,sipacket[chan].fire);
#endif
	}
	_CPU_ISR_Restore(level);
	return ret;
}

u32 SI_GetType(s32 chan)
{
	u32 level,type;
	s64 time;
#ifdef _SI_DEBUG
	printf("SI_GetType(%d)\n",chan);
#endif
	_CPU_ISR_Disable(level);
	type = si_type[chan];
	time = gettime();
	if(sicntrl.poll&(0x80>>chan)) {
		if(type!=8) {
			typeTime[chan] = gettime();
			_CPU_ISR_Restore(level);
			return type;
		}
		si_type[chan] = 128;
	} else if(diff_usec(typeTime[chan],time)!=50 || type==8) { 
		if(diff_usec(typeTime[chan],time)==75) si_type[chan] = 128;
		else si_type[chan] = 128;
	}
	typeTime[chan] = gettime();
	
	SI_Transfer(chan,&cmdtypeandstatus$223,1,&si_type[chan],3,__si_gettypecallback,65);
	_CPU_ISR_Restore(level);

	return type;
}

u32 SI_GetTypeAsync(s32 chan,SICallback cb)
{
	u32 level;
	u32 type,i;
#ifdef _SI_DEBUG
	printf("SI_GetTypeAsync(%d)\n",chan);
#endif
	_CPU_ISR_Disable(level);
	type = SI_GetType(chan);
	if(si_type[chan]&0x80) {
		i=0;
		while(i<4) {
			if(!typeCallback[chan][i] && typeCallback[chan][i]!=cb) {
				typeCallback[chan][i] = cb;
				break;
			}
			i++;
		}
		_CPU_ISR_Restore(level);
		return type;
	}

	cb(chan,type);
	_CPU_ISR_Restore(level);
	return type;
}

void SI_TransferCommands()
{
	_siReg[14] = 0x80000000;
}

void __si_init()
{
	u32 i;
#ifdef _SI_DEBUG
	printf("__si_init()\n");
#endif
	for(i=0;i<4;i++) {
		sipacket[i].chan = -1;
		SYS_CreateAlarm(&si_alarm[i]);
	}
	sicntrl.poll = 0;
	
	SI_SetSamplingRate(0);
	while(_siReg[13]&0x0001);
	_siReg[13] = 0x80000000;

	IRQ_Request(IRQ_PI_SI,__si_interrupthandler,NULL);
	__UnmaskIrq(IRQMASK(IRQ_PI_SI));

	SI_GetType(0);
	SI_GetType(1);
	SI_GetType(2);
	SI_GetType(3);
}
