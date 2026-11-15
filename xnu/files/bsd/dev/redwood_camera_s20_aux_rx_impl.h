/* Bounded 2LA native RAW10 -> CSIS4 C-PHY3 -> WDMA0, unpacked16-bit.
 * Included after the existing S20 hardware helpers. No automatic invocation.
 * All writes journaled; DMA memory is retained if abort is not acknowledged. */
#include "redwood_camera_s20_aux_journal.h"
enum ax_region {AX_TOP,AX_CMU,AX_PMU,AX_SYS,AX_CSI,AX_PHY,AX_BIAS,AX_DMA,AX_MMU0,AX_MMU1,AX_MUX,AX_REGIONS};
struct ax_receiver {
    volatile uint8_t *bank[AX_REGIONS];struct ax_journal journal;struct ax_io io;
    void *buffer;uint64_t phys;unsigned bytes,total;bool allocated,armed,ready,retained,exported,phy_touched;
    uint32_t irq,error,link0,link1,frames,changed,guard_bad,pixel_min,pixel_max;
    const char *phase;
};
static bool ax_read(void *p,unsigned r,unsigned off,uint32_t *v){
    struct ax_receiver *x=p;if(r>=AX_REGIONS||!x->bank[r])return false;*v=cam_s20_rd(x->bank[r],off);return true;
}
static bool ax_write(void *p,unsigned r,unsigned off,uint32_t v){
    struct ax_receiver *x=p;if(r>=AX_REGIONS||!x->bank[r])return false;cam_s20_wr(x->bank[r],off,v);return true;
}
static uint32_t ax_rd(struct ax_receiver *x,unsigned r,unsigned off){return cam_s20_rd(x->bank[r],off);}
static bool ax_wait_clock(struct ax_receiver *x,unsigned off){
    for(unsigned i=0;i<1000;i++){if(!(ax_rd(x,AX_TOP,off)&(1U<<16)))return true;delay_for_interval(1,NSEC_PER_USEC);}return false;
}
static bool ax_receiver_begin(struct ax_receiver *x){
    static const uint32_t physical[AX_REGIONS]={CMU_TOP_PHYS,CMU_CSIS_PHYS,PMU_PHYS,ISPPRE_SYSREG_PHYS,
        0x17070000,0x170d2300,0x170d1000,0x170c0000,0x17110000,0x17140000,0x17020414};
    static const unsigned lengths[AX_REGIONS]={0x4000,0x8000,0x4000,4,0x1000,0x500,0x100,0x5000,0x100,0x100,4};
    const unsigned width=4032,height=3024,guard=16384;
    x->phase="receiver map";x->io=(struct ax_io){x,ax_read,ax_write};
    for(unsigned i=0;i<AX_REGIONS;i++){x->bank[i]=(volatile uint8_t *)ml_io_map(physical[i],lengths[i]);if(!x->bank[i])return false;}
    x->phase="CSIS domain/PHY rail prerequisites";
    if(!(ax_rd(x,AX_PMU,0x1d84)&1U))return false;
    uint8_t rail5=0,rail6=0;
    if(redwood_spmic_read(1,0x2e,&rail5)||redwood_spmic_read(1,0x2f,&rail6)||!(rail5&0xc0)||!(rail6&0xc0))return false;
    if(cam_s20_smc4(SMC_PREPARE_PD_ONOFF,1,CSIS_NEED_SMC,2))return false;
#define AXW(r,o,m,v) do{if(!ax_jwrite(&x->journal,x->io,r,o,m,v))return false;}while(0)
    x->phase="CSIS clocks";
    static const unsigned qch[4]={0x3058,0x3078,0x307c,0x3070};
    for(unsigned i=0;i<4;i++)AXW(AX_CMU,qch[i],7U,2U);
    cam_s20_wait_ms(2);
    AXW(AX_TOP,0x104c,3U,1U);if(!ax_wait_clock(x,0x104c))return false;
    AXW(AX_TOP,0x1848,15U,0U);if(!ax_wait_clock(x,0x1848))return false;
    AXW(AX_PMU,0x730,1U,1U);cam_s20_wait_ms(1);
    x->phase="idle SysMMU/DMA prerequisites";
    static const unsigned mmu_gates[4]={0x20e4,0x20e8,0x20ec,0x20f0};
    for(unsigned i=0;i<4;i++)if(!(ax_rd(x,AX_CMU,mmu_gates[i])&(1U<<21)))return false;
    for(unsigned i=AX_MMU0;i<=AX_MMU1;i++)if((ax_rd(x,i,0x34)>>28)!=7||(ax_rd(x,i,0)&3U))return false;
    if(ax_rd(x,AX_DMA,0x20)&(1U<<31))return false;
    for(unsigned ch=1;ch<=4;ch++)for(unsigned vc=0;vc<4;vc++){
        unsigned off=ch*0x1000+vc*0x100;if((ax_rd(x,AX_DMA,off)|ax_rd(x,AX_DMA,off+0x90))&1U)return false;
    }
    if((ax_rd(x,AX_CSI,4)&1U)||ax_rd(x,AX_DMA,0x1408)||ax_rd(x,AX_CSI,0x14)||ax_rd(x,AX_CSI,0x1c))return false;
    x->phase="guarded low DMA allocation";x->bytes=width*height*2;
    x->total=(x->bytes+2*guard+guard-1)&~(guard-1);
    if(!redwood_camera_dma_get(x->total,&x->phys,&x->buffer))return false;x->allocated=true;
    if(!x->phys||x->phys+x->total>(1ULL<<32))return false;
    memset(x->buffer,0xa5,x->total);__asm__ volatile("dsb sy":::"memory");
    /* Snapshot BEFORE either digital or analog reset; reset itself can clear
     * writable fields even when the following setup never reaches them. */
    x->phase="receiver pre-reset snapshot";
    static const unsigned csi_snapshot[]={4,8,0x10,0x18,0x40,0x44,0x50,0x54,0x60,0x64,0x70,0x74,0x600,0x690,0x704};
    for(unsigned i=0;i<sizeof(csi_snapshot)/sizeof(csi_snapshot[0]);i++){
        unsigned off=csi_snapshot[i];uint32_t mask=off==4?~2U:off==0x704?0x1fU:~0U;
        if(!ax_jremember(&x->journal,x->io,AX_CSI,off,mask))return false;
    }
    if(!ax_jremember(&x->journal,x->io,AX_SYS,0,1U<<2))return false;
    static const unsigned phy_snapshot[]={0,4,8,0xc,0x10,0x14,0x18,0x1c,0x20,0x24,0x30,0x34,0x40,0x64,0x68};
    for(unsigned lane=0;lane<3;lane++)for(unsigned i=0;i<15;i++){
        unsigned off=phy_snapshot[i];uint32_t mask=off==0x30?0x1ffU:off==0x40?1U:~0U;
        if(!ax_jremember(&x->journal,x->io,AX_PHY,0x100+lane*0x100+off,mask))return false;
    }
    for(unsigned i=0;i<4;i++)if(!ax_jremember(&x->journal,x->io,AX_BIAS,i==3?0x10:i*4,~0U))return false;
    x->phase="C-PHY4 reset/configuration";
    AXW(AX_CSI,4,1U,0);
    cam_s20_wr(x->bank[AX_CSI],4,ax_rd(x,AX_CSI,4)|2U);
    for(unsigned i=0;i<10&&(ax_rd(x,AX_CSI,4)&2U);i++)delay_for_interval(10,NSEC_PER_USEC);
    if(ax_rd(x,AX_CSI,4)&2U)return false;
    x->phy_touched=true;AXW(AX_SYS,0,1U<<2,0);
    for(unsigned lane=0;lane<3;lane++)AXW(AX_PHY,0x100+lane*0x100,~0U,0);
    delay_for_interval(200,NSEC_PER_USEC);
    AXW(AX_BIAS,0,~0U,0x10);AXW(AX_BIAS,4,~0U,0x110);AXW(AX_BIAS,8,~0U,0x3223);AXW(AX_BIAS,0x10,~0U,0x40);
    static const unsigned po[11]={4,8,0xc,0x10,0x14,0x18,0x1c,0x20,0x24,0x64,0x68};
    static const uint32_t pv[11]={0x1450,9,0x82c8,1,0x8680,0x4000,0x200,0x638,0x40,0x1503,0x33};
    for(unsigned lane=0;lane<3;lane++){
        unsigned off=0x100+lane*0x100;
        for(unsigned i=0;i<11;i++)AXW(AX_PHY,off+po[i],~0U,pv[i]);
        AXW(AX_PHY,off+0x30,0x1ffU,7U);AXW(AX_PHY,off+0x34,~0U,0x32);
        AXW(AX_PHY,off+0x40,1U,0);AXW(AX_PHY,off,~0U,1);
    }
    delay_for_interval(200,NSEC_PER_USEC);AXW(AX_SYS,0,1U<<2,1U<<2);
    x->phase="CSIS4 VC0 RAW10";
    AXW(AX_CSI,4,(7U<<13)|(3U<<10)|(3U<<8)|(1U<<21)|1U,(1U<<21)|(3U<<10)|(2U<<8));
    AXW(AX_CSI,8,1U<<4,1U<<4);
    AXW(AX_CSI,0x704,0x1fU,0x1eU);
    for(unsigned vc=0;vc<4;vc++){
        AXW(AX_CSI,0x40+vc*0x10,~0U,(vc<<16)|(2U<<12)|(vc?0:0x2bU<<2));
        AXW(AX_CSI,0x44+vc*0x10,~0U,vc?0:(height<<16)|width);
    }
    AXW(AX_CSI,0x600,1U<<31,0); /* 2LA DT LRTE_DISABLE */
    AXW(AX_CSI,0x690,1U<<23,1U<<23);
    cam_s20_wr(x->bank[AX_CSI],0xc,0xf); /* self-clearing shadow update */
    AXW(AX_CSI,0x10,~0U,0x00fff07f);AXW(AX_CSI,0x18,~0U,0x7f);
    x->phase="WDMA0 unpacked16 input";
    AXW(AX_MUX,0,~0U,4U);
    AXW(AX_DMA,8,2U,2U);AXW(AX_DMA,0x1414,1U,0);
    AXW(AX_DMA,0x1400,1U,0);AXW(AX_DMA,0x141c,3U,0);
    AXW(AX_DMA,0x1404,~0U,0x01111110);
    AXW(AX_DMA,0x1004,0x7fU,6U);
    AXW(AX_DMA,0x1008,~0U,0);AXW(AX_DMA,0x100c,~0U,0);AXW(AX_DMA,0x10a4,~0U,width);
    for(unsigned i=0;i<32;i++)AXW(AX_DMA,0x1010+i*4,~0U,(uint32_t)(x->phys+guard));
    AXW(AX_DMA,0x10b0,~0U,~0U);
    AXW(AX_DMA,0x24,~0U,0x00804640);AXW(AX_DMA,0x28,~0U,(height<<16)|width);AXW(AX_DMA,0x20,~0U,0x400fffff);
    x->armed=true; /* BEFORE enabling: a failed readback may follow a latched write */
    AXW(AX_DMA,0x1000,1U,1U);
    cam_s20_wr(x->bank[AX_DMA],0x1000,ax_rd(x,AX_DMA,0x1000)|2U);
    AXW(AX_CSI,0x704,1U,1U);AXW(AX_CSI,4,1U,1U);
    x->ready=true;x->phase="receiver armed";return true;
#undef AXW
}
static bool ax_receiver_end(struct ax_receiver *x){
    bool clean=true,stopped=!x->armed;
    if(x->armed){
        x->irq=ax_rd(x,AX_DMA,0x1408);x->error=ax_rd(x,AX_DMA,0x1418);
        x->link0=ax_rd(x,AX_CSI,0x14);x->link1=ax_rd(x,AX_CSI,0x1c);x->frames=ax_rd(x,AX_CSI,0x100);
        cam_s20_wr(x->bank[AX_CSI],4,ax_rd(x,AX_CSI,4)&~1U);
        cam_s20_wr(x->bank[AX_DMA],0x1000,ax_rd(x,AX_DMA,0x1000)&~1U);
        cam_s20_wr(x->bank[AX_DMA],0x1414,ax_rd(x,AX_DMA,0x1414)|1U);
        for(unsigned i=0;i<100;i++){
            if((ax_rd(x,AX_DMA,0x1408)&(1U<<16))&&!(ax_rd(x,AX_DMA,0x1090)&1U)){stopped=true;break;}
            cam_s20_wait_ms(1);
        }
        if(!stopped){x->retained=true;x->phase="DMA abort unacknowledged; memory retained";return false;}
    }
    __asm__ volatile("dsb sy":::"memory");
    if(x->ready&&x->allocated){
        const unsigned guard=16384;
        volatile uint8_t *b=x->buffer;
        for(unsigned i=0;i<x->total;i++)if((i<guard||i>=guard+x->bytes)&&b[i]!=0xa5)x->guard_bad++;
        volatile uint16_t *pixels=(volatile uint16_t *)(b+guard);x->pixel_min=0xffff;
        for(unsigned i=0;i<x->bytes/2;i++){
            unsigned v=pixels[i];if(v<x->pixel_min)x->pixel_min=v;if(v>x->pixel_max)x->pixel_max=v;
            if(v!=0xa5a5)x->changed++;
        }
        bool captured=(x->irq&(1U<<8))&&!(x->irq&0x01101000U)&&!x->error&&
            !(x->link0&0x00fff07fU)&&!(x->link1&0x78U)&&!x->guard_bad&&x->changed&&x->pixel_max<=1023;
        if(captured)x->exported=cam_s20_save_frame_named(pixels,4032,3024,true,"S5K2LA");
        x->phase=x->exported?"RAW frame exported":"no clean RAW frame";
    }
    if(x->armed){
        cam_s20_wr(x->bank[AX_DMA],0x1408,ax_rd(x,AX_DMA,0x1408));
        cam_s20_wr(x->bank[AX_CSI],0x14,ax_rd(x,AX_CSI,0x14));
        cam_s20_wr(x->bank[AX_CSI],0x1c,ax_rd(x,AX_CSI,0x1c));
    }
    if(x->phy_touched)cam_s20_wr(x->bank[AX_SYS],0,ax_rd(x,AX_SYS,0)&~(1U<<2));
    if(!ax_jrestore(&x->journal,x->io))clean=false;
    if(x->bank[AX_TOP]&&(!ax_wait_clock(x,0x104c)||!ax_wait_clock(x,0x1848)))clean=false;
    if(x->allocated){redwood_camera_dma_put();x->allocated=false;}
    return clean;
}
