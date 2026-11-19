/* FolioOS — draws the macOS app icon (the splash mark: a sheet with a folded corner and four
 * lines) into an .iconset folder; build_macos.sh turns it into an .icns with iconutil.
 * Same colours and proportions as draw() in src/core/splash.c, so no image file is kept in the repo.
 *   mkicon_macos OUT.iconset
 */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <stdio.h>

static CGColorRef rgb(CGColorSpaceRef cs, unsigned v, float a) {
    CGFloat c[4] = { ((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0, (v & 255) / 255.0, a };
    return CGColorCreate(cs, c);
}

static void fill(CGContextRef g, CGColorSpaceRef cs, unsigned v) {
    CGColorRef c = rgb(cs, v, 1); CGContextSetFillColorWithColor(g, c); CGContextFillPath(g); CGColorRelease(c);
}

static void rrect(CGContextRef g, CGRect r, CGFloat rad) {
    CGPathRef p = CGPathCreateWithRoundedRect(r, rad, rad, NULL); CGContextAddPath(g, p); CGPathRelease(p);
}

/* draws the icon at size px × px; coordinates below are on a 1024 grid, y pointing down */
static CGImageRef draw(int px) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef g = CGBitmapContextCreate(NULL, px, px, 8, 0, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGContextScaleCTM(g, px / 1024.0, px / 1024.0);
    CGContextTranslateCTM(g, 0, 1024); CGContextScaleCTM(g, 1, -1);

    /* macOS icon grid: 824 × 824 rounded square centred on the 1024 canvas, soft drop shadow */
    CGRect tile = CGRectMake(100, 100, 824, 824);
    CGColorRef sh = rgb(cs, 0x000000, 0.35f);
    CGContextSaveGState(g);
    CGContextSetShadowWithColor(g, CGSizeMake(0, -12), 28, sh);
    rrect(g, tile, 185); fill(g, cs, 0x1C1E24);
    CGContextRestoreGState(g);
    CGContextSaveGState(g);
    rrect(g, tile, 185); CGContextClip(g);
    CGColorRef stops[2] = { rgb(cs, 0x1C1E24, 1), rgb(cs, 0x0C0D10, 1) };
    CFArrayRef arr = CFArrayCreate(NULL, (const void **)stops, 2, &kCFTypeArrayCallBacks);
    CGGradientRef grad = CGGradientCreateWithColors(cs, arr, NULL);
    CGContextDrawLinearGradient(g, grad, CGPointMake(0, 100), CGPointMake(0, 924), 0);
    CGGradientRelease(grad); CFRelease(arr); CGColorRelease(stops[0]); CGColorRelease(stops[1]);

    /* the sheet: splash.c units × sc (66 × 86, corner 5, fold 18) */
    const CGFloat sc = 6.2, pw = 66 * sc, ph = 86 * sc, rad = 5 * sc, fold = 18 * sc;
    const CGFloat x = 512 - pw / 2, y = 512 - ph / 2, rx = x + pw, by = y + ph;
    CGMutablePathRef page = CGPathCreateMutable();
    CGPathMoveToPoint(page, NULL, x + rad, y);
    CGPathAddLineToPoint(page, NULL, rx - fold, y);
    CGPathAddLineToPoint(page, NULL, rx, y + fold);
    CGPathAddArcToPoint(page, NULL, rx, by, rx - rad, by, rad);
    CGPathAddArcToPoint(page, NULL, x, by, x, by - rad, rad);
    CGPathAddArcToPoint(page, NULL, x, y, x + rad, y, rad);
    CGPathCloseSubpath(page);
    CGColorRef psh = rgb(cs, 0x000000, 0.44f);
    CGContextSaveGState(g);
    CGContextSetShadowWithColor(g, CGSizeMake(0, -6 * sc), 24 * sc * 0.6, psh);
    CGContextAddPath(g, page); fill(g, cs, 0xF4EEE1);
    CGContextRestoreGState(g);
    CGPathRelease(page); CGColorRelease(psh);

    /* folded flap: mix(PAPER, 0x8C8273, 0.55) as in splash.c */
    CGContextMoveToPoint(g, rx - fold, y);
    CGContextAddLineToPoint(g, rx - fold, y + fold);
    CGContextAddLineToPoint(g, rx, y + fold);
    CGContextClosePath(g); fill(g, cs, 0xBBB3A5);

    /* four lines of text, the first one in the accent colour */
    for (int k = 0; k < 4; k++) {
        CGFloat lw = k == 0 ? pw * 0.42 : k == 3 ? pw * 0.38 : pw * 0.64;
        rrect(g, CGRectMake(x + 11 * sc, y + (24 + k * 13) * sc, lw, 4.5 * sc), 2 * sc);
        fill(g, cs, k == 0 ? 0xE2A65A : 0x5A5248);
    }
    /* the caret after the last line */
    CGContextAddRect(g, CGRectMake(x + 13 * sc + pw * 0.38, y + (24 + 3 * 13 - 4) * sc, 2 * sc, 12 * sc));
    fill(g, cs, 0xE2A65A);
    CGContextRestoreGState(g);

    CGImageRef img = CGBitmapContextCreateImage(g);
    CGColorRelease(sh); CGContextRelease(g); CGColorSpaceRelease(cs);
    return img;
}

static bool save(CGImageRef img, NSString *path) {
    CGImageDestinationRef d = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],
                                                              CFSTR("public.png"), 1, NULL);
    if (!d) return false;
    CGImageDestinationAddImage(d, img, NULL);
    bool ok = CGImageDestinationFinalize(d);
    CFRelease(d);
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: mkicon_macos OUT.iconset\n"); return 2; }
    @autoreleasepool {
        NSString *dir = [NSString stringWithUTF8String:argv[1]];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        static const int sizes[] = { 16, 32, 128, 256, 512 };
        for (int i = 0; i < 5; i++) for (int s = 1; s <= 2; s++) {
            CGImageRef img = draw(sizes[i] * s);
            NSString *name = s == 1 ? [NSString stringWithFormat:@"icon_%dx%d.png", sizes[i], sizes[i]]
                                    : [NSString stringWithFormat:@"icon_%dx%d@2x.png", sizes[i], sizes[i]];
            bool ok = save(img, [dir stringByAppendingPathComponent:name]);
            CGImageRelease(img);
            if (!ok) { fprintf(stderr, "mkicon_macos: cannot write %s\n", name.UTF8String); return 1; }
        }
    }
    return 0;
}
