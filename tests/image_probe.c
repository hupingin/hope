/* Standalone harness: decode every generated image and compare against the
 * reference pixels PIL wrote next to each file. Not part of the shipped binary;
 * built ad hoc from the command line to validate hpe_image.c. */
#include "render/hpe_image.h"
#include "base/hpe_util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int compare(const char *img_path, int *out_bad, int *out_total, int *out_maxdiff,
                   double *out_avgdiff)
{
    char raw_path[512];
    HpeImage img;
    FILE *f;
    long n;
    unsigned char *ref;
    int i, bad = 0, maxdiff = 0;
    double sum = 0;

    memset(&img, 0, sizeof(img));
    if (!hpe_image_load_file(img_path, HPE_IMG_UNKNOWN, &img)) {
        printf("  %-22s DECODE FAILED\n", img_path + strlen(img_path) - 20);
        return 0;
    }
    snprintf(raw_path, sizeof(raw_path), "%s.raw", img_path);
    f = fopen(raw_path, "rb");
    if (!f) {
        printf("  no reference for %s\n", img_path);
        hpe_image_free(&img);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    ref = (unsigned char *)malloc((size_t)n);
    if (fread(ref, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(ref);
        hpe_image_free(&img);
        return 0;
    }
    fclose(f);

    if (n != (long)img.width * img.height * 4) {
        printf("  %-22s SIZE MISMATCH got %dx%d (%ld ref bytes)\n", img_path, img.width,
               img.height, n);
        free(ref);
        hpe_image_free(&img);
        return 0;
    }
    for (i = 0; i < img.width * img.height; i++) {
        uint32_t p = img.pixels[i];
        int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF, a = (p >> 24) & 0xFF;
        int rr = ref[i * 4], rg = ref[i * 4 + 1], rb = ref[i * 4 + 2], ra = ref[i * 4 + 3];
        int d = abs(r - rr) + abs(g - rg) + abs(b - rb) + abs(a - ra);
        sum += d;
        if (d > maxdiff)
            maxdiff = d;
        if (d > 40)
            bad++;
    }
    *out_bad = bad;
    *out_total = img.width * img.height;
    *out_maxdiff = maxdiff;
    *out_avgdiff = sum / (double)(img.width * img.height);
    free(ref);
    hpe_image_free(&img);
    return 1;
}

int main(int argc, char **argv)
{
    int i, fail = 0;
    const char *dir = argc > 1 ? argv[1] : "build/imgtest";
    const char *names[] = {"rgba32.png",       "rgb24.png",    "gray.png",   "pal.png",
                           "bw.png",           "rgb16.png",    "interlaced.png",
                           "rgb.jpg",          "prog.jpg",     "gray.jpg",   "anim.gif",
                           "rgb.gif",          "rgb24.bmp",    "gray.bmp",   "pal8.bmp"};
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        char path[512];
        int bad = 0, total = 0, maxdiff = 0;
        double avg = 0;
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (!compare(path, &bad, &total, &maxdiff, &avg)) {
            fail = 1;
            continue;
        }
        printf("  %-16s %dx%d  bad=%d/%d maxdiff=%d avg=%.2f %s\n", names[i], 0, 0, bad, total,
               maxdiff, avg, bad == 0 ? "OK" : "MISMATCH");
        if (bad != 0)
            fail = 1;
    }
    printf("%s\n", fail ? "IMAGE TESTS FAILED" : "IMAGE TESTS PASSED");
    return fail;
}
