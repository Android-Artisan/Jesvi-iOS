/* Evaluate the production black interpolation against an independent covered RAW.
 * Host only: no device access. Signed residuals are measured BEFORE clipping. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../../userland/src/camera_isp.c"
int main(int argc, char **argv) {
    if (argc!=3) return 2;
    struct isp_black_calibration cal;
    struct isp isp;
    isp_init(&isp);
    if (!isp_load_black_calibration(&cal,argv[1])) return 2;
    FILE *fp=fopen(argv[2],"rb");
    if (!fp) return 2;
    struct redwood_camera_frame f;
    if (fread(&f,sizeof(f),1,fp)!=1 || memcmp(f.magic,"RWFRAME1",8) || f.header_bytes!=sizeof(f) ||
        f.width!=2016 || f.height!=1512 || f.stride!=2528) return 2;
    size_t bytes=(size_t)f.stride*f.height;
    uint8_t *data=malloc(bytes);
    if (!data || fread(data,1,bytes,fp)!=bytes || fgetc(fp)!=EOF) return 2;
    fclose(fp);
    if (!isp_apply_black_calibration(&isp,&cal,&f)) return 2;
    double sum[4]={0},sq[4]={0},old[4]={0};
    unsigned n[4]={0};
    double max_tile=0;
    /* Existing inside-table values for the measured 8x holdout. */
    const double previous[4]={28,39,30,37};
    for(unsigned by=0;by<13;by++) for(unsigned bx=0;bx<17;bx++) {
        double tile[4]={0}; unsigned count[4]={0};
        unsigned y0=(f.height*by/13)&~1u,y1=(f.height*(by+1)/13)&~1u;
        unsigned x0=(f.width*bx/17)&~3u,x1=(f.width*(bx+1)/17)&~3u;
        for(unsigned y=y0;y<y1;y+=4) for(unsigned x=x0;x<x1;x+=8) {
            for(unsigned dy=0;dy<2;dy++) for(unsigned dx=0;dx<2;dx++) {
                unsigned k=dy*2+dx;
                const uint8_t *g=data+(y+dy)*f.stride+(x/4)*5;
                int v=(g[dx]<<2)|((g[4]>>(dx*2))&3);
                double residual=v-black_at(&isp,&f,x+dx,y+dy);
                sum[k]+=residual; sq[k]+=residual*residual; old[k]+=v-previous[k]; n[k]++;
                tile[k]+=residual; count[k]++;
            }
        }
        for(unsigned k=0;k<4;k++) if(fabs(tile[k]/count[k])>max_tile) max_tile=fabs(tile[k]/count[k]);
    }
    for(unsigned k=0;k<4;k++) printf("site %u: old bias %.3f -> corrected %.3f; residual RMS %.3f\n",k,old[k]/n[k],sum[k]/n[k],sqrt(sq[k]/n[k]));
    printf("maximum absolute corrected tile mean %.3f RAW counts\n",max_tile);
    free(data);
    return max_tile<5 ? 0:1;
}
