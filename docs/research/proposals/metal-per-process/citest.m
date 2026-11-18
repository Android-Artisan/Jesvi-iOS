// citest: CoreImage on RedwoodMetal, the paths Photos' wallpaper editor takes.
// DYLD_INSERT_LIBRARIES=/var/jb/usr/lib/libredwood-metalon.dylib ./citest [image]
#import <Foundation/Foundation.h>
#import <CoreImage/CoreImage.h>
#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
int main(int argc, char **argv) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    printf("device=%s\n", dev ? [[dev name] UTF8String] : "nil");
    if (!dev) return 1;
    CIContext *ctx = getenv("CPU") ? [CIContext contextWithOptions:@{kCIContextUseSoftwareRenderer:@YES}] : [CIContext contextWithMTLDevice:dev];
    const char *only = getenv("ONLY");
    CIImage *img;
    if (argc > 1) img = [CIImage imageWithContentsOfURL:[NSURL fileURLWithPath:@(argv[1])]];
    else img = [[CIImage imageWithColor:[CIColor colorWithRed:0.2 green:0.5 blue:0.8]] imageByCroppingToRect:CGRectMake(0,0,837,629)];
    printf("image %.0fx%.0f\n", img.extent.size.width, img.extent.size.height);
    const char *steps[] = {"CIAreaAverage","CIGaussianBlur","CIColorControls","CIVibrance","CIExposureAdjust","CIAreaMinMax","CIHighlightShadowAdjust", NULL};
    for (int i = 0; steps[i]; i++) {
        if (only && strcmp(only, steps[i])) continue;
        CIFilter *f = [CIFilter filterWithName:@(steps[i])];
        [f setValue:img forKey:kCIInputImageKey];
        if ([f.inputKeys containsObject:kCIInputExtentKey]) [f setValue:[CIVector vectorWithCGRect:img.extent] forKey:kCIInputExtentKey];
        CIImage *out = f.outputImage;
        CGRect r = CGRectIntersection(out.extent, img.extent); if (CGRectIsEmpty(r)) r = out.extent;
        if (CGRectIsInfinite(r)) r = img.extent;
        uint8_t px[4] = {0};
        [ctx render:out toBitmap:px rowBytes:4 bounds:CGRectMake(r.origin.x, r.origin.y, 1, 1) format:kCIFormatRGBA8 colorSpace:nil];
        CGImageRef cg = [ctx createCGImage:out fromRect:r];
        printf("%-24s ok  px=%u,%u,%u,%u cg=%zux%zu\n", steps[i], px[0],px[1],px[2],px[3], cg?CGImageGetWidth(cg):0, cg?CGImageGetHeight(cg):0);
        if (cg) CGImageRelease(cg);
    }
    printf("done\n");
} return 0; }
