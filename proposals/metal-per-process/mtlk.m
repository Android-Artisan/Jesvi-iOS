// mtlk: run MPS's compute_intermediate_mean_float_statistics through the Metal API on our driver.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
int main(int argc, char **argv) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@"/System/Library/Frameworks/MetalPerformanceShaders.framework/Frameworks/MPSImage.framework/default.metallib"] error:&err];
    id<MTLFunction> fn = [lib newFunctionWithName:@"compute_intermediate_mean_float_statistics"];
    id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:fn error:&err];
    printf("pipeline %p err %s\n", ps, err ? [[err description] UTF8String] : "-");
    unsigned W = argc > 1 ? atoi(argv[1]) : 64, H = argc > 2 ? atoi(argv[2]) : 320;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:W height:H mipmapped:NO];
    d.usage = getenv("RO") ? MTLTextureUsageShaderRead : MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    if (getenv("H16")) d.pixelFormat = MTLPixelFormatRGBA16Float;
    id<MTLTexture> in = [dev newTextureWithDescriptor:d];
    float *px = calloc(W * H * 4, sizeof(float));
    for (unsigned i = 0; i < W * H; i++) { px[i*4] = 1; px[i*4+1] = 2; px[i*4+2] = 3; px[i*4+3] = 4; }
    if (getenv("H16")) { uint16_t *hp = calloc(W*H*4,2); for (unsigned i=0;i<W*H;i++){hp[i*4]=0x3c00;hp[i*4+1]=0x4000;hp[i*4+2]=0x4200;hp[i*4+3]=0x4400;} [in replaceRegion:MTLRegionMake2D(0,0,W,H) mipmapLevel:0 withBytes:hp bytesPerRow:W*8]; }
    else [in replaceRegion:MTLRegionMake2D(0,0,W,H) mipmapLevel:0 withBytes:px bytesPerRow:W*16];
    { float chk[4]={0}; [in getBytes:chk bytesPerRow:W*16 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0]; printf("in(0,0) readback %g %g %g %g\n", chk[0],chk[1],chk[2],chk[3]); }
    MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:H height:2 mipmapped:NO];
    od.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> out = [dev newTextureWithDescriptor:od];
    { float *s7 = calloc(H*2*4, sizeof(float)); for (unsigned i=0;i<H*2*4;i++) s7[i]=7; [out replaceRegion:MTLRegionMake2D(0,0,H,2) mipmapLevel:0 withBytes:s7 bytesPerRow:H*16]; }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:ps];
    [e setTexture:out atIndex:0];
    [e setTexture:in atIndex:1];
    // ubo: offset 8 holds (count.x, count.y) as ushort2; give width,height
    uint16_t ub[16] = {0}; ub[4] = W; ub[5] = H; ub[0] = W; ub[1] = H; ub[2]=W; ub[3]=H; ub[6]=W; ub[7]=H;
    if (getenv("ZERO")) memset(ub, 0, sizeof(ub));
    [e setBytes:ub length:sizeof(ub) atIndex:0];
    [e dispatchThreadgroups:MTLSizeMake((H+7)/8,1,1) threadsPerThreadgroup:MTLSizeMake(8,1,1)];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    printf("status %ld err %s\n", (long)cb.status, cb.error ? [[cb.error description] UTF8String] : "-");
    float o[8] = {0};
    [out getBytes:o bytesPerRow:H*16 fromRegion:MTLRegionMake2D(0,0,2,1) mipmapLevel:0];
    printf("out(0,0)=%g %g %g %g  out(1,0)=%g %g %g %g  (expect %u*1..4)\n", o[0],o[1],o[2],o[3],o[4],o[5],o[6],o[7], W);
} return 0; }
