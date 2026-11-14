// glyphtest: what a glyph cache does -- small sub-rectangle uploads into an R8 / RGBA8 atlas, read back, compare.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
static int run(id<MTLDevice> dev, MTLPixelFormat pf, unsigned bpp, unsigned W, unsigned H, MTLStorageMode sm) {
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:W height:H mipmapped:NO];
    d.usage=MTLTextureUsageShaderRead; d.storageMode=sm;
    id<MTLTexture> t=[dev newTextureWithDescriptor:d];
    if(!t){printf("fmt %lu: no texture\n",(unsigned long)pf);return 1;}
    uint8_t *ref=calloc(W*H*bpp,1);
    unsigned seed=1, bad=0, tests=0;
    for(int k=0;k<200;k++){
        unsigned w=1+(seed=seed*1103515245+12345)%37, h=1+(seed=seed*1103515245+12345)%29;
        unsigned x=(seed=seed*1103515245+12345)%(W-w), y=(seed=seed*1103515245+12345)%(H-h);
        unsigned row=w*bpp + ((k&1)?0:((seed>>8)%13));       // tight and padded rows
        uint8_t *src=malloc(row*h);
        for(unsigned i=0;i<row*h;i++) src[i]=(uint8_t)(k*31+i*7);
        [t replaceRegion:MTLRegionMake2D(x,y,w,h) mipmapLevel:0 withBytes:src bytesPerRow:row];
        for(unsigned j=0;j<h;j++) memcpy(ref+((y+j)*W+x)*bpp, src+j*row, w*bpp);
        free(src);
    }
    uint8_t *out=calloc(W*H*bpp,1);
    [t getBytes:out bytesPerRow:W*bpp fromRegion:MTLRegionMake2D(0,0,W,H) mipmapLevel:0];
    for(unsigned i=0;i<W*H*bpp;i++){ tests++; if(out[i]!=ref[i]){ if(bad<5) printf("  fmt %lu %ux%u: byte %u (x %u y %u) is %u want %u\n",(unsigned long)pf,W,H,i,(i/bpp)%W,(i/bpp)/W,out[i],ref[i]); bad++; } }
    printf("fmt %lu %ux%u storage %lu: %u of %u bytes wrong\n",(unsigned long)pf,W,H,(unsigned long)sm,bad,tests);
    return bad!=0;
}
int main(void){ setvbuf(stdout,NULL,_IONBF,0); @autoreleasepool{
    id<MTLDevice> dev=MTLCreateSystemDefaultDevice(); printf("device %s\n",[[dev name] UTF8String]);
    run(dev,MTLPixelFormatR8Unorm,1,512,512,MTLStorageModeShared);
    run(dev,MTLPixelFormatR8Unorm,1,256,128,MTLStorageModeShared);
    run(dev,MTLPixelFormatR8Unorm,1,100,77,MTLStorageModeShared);
    run(dev,MTLPixelFormatRGBA8Unorm,4,512,512,MTLStorageModeShared);
    run(dev,MTLPixelFormatBGRA8Unorm,4,300,200,MTLStorageModeShared);
    run(dev,MTLPixelFormatR8Unorm,1,512,512,MTLStorageModePrivate);
    /* ASTC 4x4: 16x16 texels = 4x4 blocks of 16 bytes, uploaded and read back */
    {
        MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)204 width:16 height:16 mipmapped:NO];
        id<MTLTexture> t=[dev newTextureWithDescriptor:d];
        uint8_t blk[16*16]; for(int i=0;i<256;i++) blk[i]=(uint8_t)(i*13+1);
        if(t){ [t replaceRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0 withBytes:blk bytesPerRow:64];
               uint8_t back[256]={0}; [t getBytes:back bytesPerRow:64 fromRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0];
               printf("ASTC 4x4: made, round trip %s\n", memcmp(blk,back,256)?"DIFFERS":"exact"); }
        else printf("ASTC 4x4: nil\n");
    }
    /* no-copy: a buffer and a texture over our own pages */
    void *page=NULL; posix_memalign(&page,16384,65536); memset(page,0x5a,65536);
    id<MTLBuffer> b=[dev newBufferWithBytesNoCopy:page length:65536 options:0 deallocator:nil];
    printf("buffer no-copy: %s, contents %s\n", b?"made":"nil", b&&[b contents]==page?"is the caller's":"differs");
    SEL s=NSSelectorFromString(@"newTextureWithBytesNoCopy:length:descriptor:deallocator:");
    if([dev respondsToSelector:s]){
        MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:256 height:256 mipmapped:NO];
        id<MTLTexture> t=((id(*)(id,SEL,void*,NSUInteger,id,id))objc_msgSend)(dev,s,page,65536,d,nil);
        ((uint8_t*)page)[257]=0x77;
        (void)t;
        printf("texture no-copy: %s\n", t?"made":"nil");
    }
} return 0; }
