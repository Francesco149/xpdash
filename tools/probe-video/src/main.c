#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static void save_bmp32(const char *filename, int w, int h, const uint32_t *pixels) {
    FILE *f = fopen(filename, "wb");
    if (!f) return;

    BITMAPFILEHEADER bfh;
    BITMAPINFOHEADER bih;
    memset(&bfh, 0, sizeof(bfh));
    memset(&bih, 0, sizeof(bih));

    int image_size = w * h * 4;
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
    bfh.bfSize = bfh.bfOffBits + image_size;

    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = -h; // top-down
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = image_size;

    fwrite(&bfh, sizeof(bfh), 1, f);
    fwrite(&bih, sizeof(bih), 1, f);
    fwrite(pixels, image_size, 1, f);
    fclose(f);
}

int main(int argc, char **argv) {
    int delay_sec = (argc > 1) ? atoi(argv[1]) : 0;
    if (delay_sec > 0) {
        printf("Waiting %d seconds before probe...\n", delay_sec);
        Sleep(delay_sec * 1000);
    }

    HDC hdc_screen = GetDC(NULL);
    if (!hdc_screen) {
        printf("ERROR: GetDC(NULL) failed!\n");
        return 1;
    }

    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int bpp = GetDeviceCaps(hdc_screen, BITSPIXEL);
    int planes = GetDeviceCaps(hdc_screen, PLANES);
    int raster_caps = GetDeviceCaps(hdc_screen, RASTERCAPS);
    int size_palette = GetDeviceCaps(hdc_screen, SIZEPALETTE);
    int num_colors = GetDeviceCaps(hdc_screen, NUMCOLORS);

    printf("=== Display Probe ===\n");
    printf("Resolution: %dx%d\n", screen_w, screen_h);
    printf("BitsPixel: %d, Planes: %d (effective bpp = %d)\n", bpp, planes, bpp * planes);
    printf("RasterCaps: 0x%04X (RC_PALETTE=%s)\n", raster_caps, (raster_caps & RC_PALETTE) ? "YES" : "NO");
    printf("SizePalette: %d, NumColors: %d\n", size_palette, num_colors);

    // Read system palette entries
    PALETTEENTRY sys_pal[256];
    memset(sys_pal, 0, sizeof(sys_pal));
    UINT pal_count = GetSystemPaletteEntries(hdc_screen, 0, 256, sys_pal);
    printf("GetSystemPaletteEntries returned: %u entries\n", pal_count);
    for (int i = 0; i < 16; i++) {
        printf("  Pal[%3d]: R=%3d G=%3d B=%3d flags=0x%02X\n",
               i, sys_pal[i].peRed, sys_pal[i].peGreen, sys_pal[i].peBlue, sys_pal[i].peFlags);
    }
    printf("  ...\n");
    for (int i = 240; i < 256; i++) {
        printf("  Pal[%3d]: R=%3d G=%3d B=%3d flags=0x%02X\n",
               i, sys_pal[i].peRed, sys_pal[i].peGreen, sys_pal[i].peBlue, sys_pal[i].peFlags);
    }

    // ── Test 1: BitBlt to 32-bit DIBSection directly (current agent logic) ──
    {
        HDC hdc_mem32 = CreateCompatibleDC(hdc_screen);
        BITMAPINFO bmi32;
        memset(&bmi32, 0, sizeof(bmi32));
        bmi32.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi32.bmiHeader.biWidth = screen_w;
        bmi32.bmiHeader.biHeight = -screen_h;
        bmi32.bmiHeader.biPlanes = 1;
        bmi32.bmiHeader.biBitCount = 32;
        bmi32.bmiHeader.biCompression = BI_RGB;

        uint32_t *pix32 = NULL;
        HBITMAP hbm32 = CreateDIBSection(hdc_screen, &bmi32, DIB_RGB_COLORS, (void **)&pix32, NULL, 0);
        HBITMAP old32 = (HBITMAP)SelectObject(hdc_mem32, hbm32);

        BOOL blt_ok = BitBlt(hdc_mem32, 0, 0, screen_w, screen_h, hdc_screen, 0, 0, SRCCOPY);
        printf("\n--- Test 1: BitBlt to 32-bit DIBSection ---\n");
        printf("BitBlt result: %s (err=%lu)\n", blt_ok ? "OK" : "FAIL", GetLastError());

        int total_pixels = screen_w * screen_h;
        int black_count = 0;
        int nonzero_count = 0;
        for (int i = 0; i < total_pixels; i++) {
            if ((pix32[i] & 0x00FFFFFF) == 0) black_count++;
            else nonzero_count++;
        }
        printf("Pixels: total=%d, black=%d (%.1f%%), nonzero=%d (%.1f%%)\n",
               total_pixels, black_count, (double)black_count * 100.0 / total_pixels,
               nonzero_count, (double)nonzero_count * 100.0 / total_pixels);

        save_bmp32("C:\\probe\\out\\probe_t1_plain32.bmp", screen_w, screen_h, pix32);
        printf("Saved probe_t1_plain32.bmp\n");

        SelectObject(hdc_mem32, old32);
        DeleteObject(hbm32);
        DeleteDC(hdc_mem32);
    }

    // ── Test 2: BitBlt to 8-bit DIBSection + expand using sys_pal LUT ──
    {
        HDC hdc_mem8 = CreateCompatibleDC(hdc_screen);
        struct {
            BITMAPINFOHEADER bmiHeader;
            RGBQUAD bmiColors[256];
        } bmi8;
        memset(&bmi8, 0, sizeof(bmi8));
        bmi8.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi8.bmiHeader.biWidth = screen_w;
        bmi8.bmiHeader.biHeight = -screen_h;
        bmi8.bmiHeader.biPlanes = 1;
        bmi8.bmiHeader.biBitCount = 8;
        bmi8.bmiHeader.biCompression = BI_RGB;
        bmi8.bmiHeader.biClrUsed = 256;

        for (int i = 0; i < 256; i++) {
            bmi8.bmiColors[i].rgbRed = sys_pal[i].peRed;
            bmi8.bmiColors[i].rgbGreen = sys_pal[i].peGreen;
            bmi8.bmiColors[i].rgbBlue = sys_pal[i].peBlue;
            bmi8.bmiColors[i].rgbReserved = 0;
        }

        uint8_t *pix8 = NULL;
        HBITMAP hbm8 = CreateDIBSection(hdc_screen, (BITMAPINFO *)&bmi8, DIB_RGB_COLORS, (void **)&pix8, NULL, 0);
        HBITMAP old8 = (HBITMAP)SelectObject(hdc_mem8, hbm8);

        BOOL blt_ok = BitBlt(hdc_mem8, 0, 0, screen_w, screen_h, hdc_screen, 0, 0, SRCCOPY);
        printf("\n--- Test 2: BitBlt to 8-bit DIBSection ---\n");
        printf("BitBlt result: %s (err=%lu)\n", blt_ok ? "OK" : "FAIL", GetLastError());

        int total_pixels = screen_w * screen_h;
        int hist[256] = {0};
        for (int i = 0; i < total_pixels; i++) {
            hist[pix8[i]]++;
        }
        int distinct_indices = 0;
        for (int i = 0; i < 256; i++) {
            if (hist[i] > 0) distinct_indices++;
        }
        printf("Distinct 8-bit indices used: %d / 256 (index 0 count: %d, %.1f%%)\n",
               distinct_indices, hist[0], (double)hist[0] * 100.0 / total_pixels);

        // Expand 8-bit to 32-bit using sys_pal LUT
        uint32_t lut[256];
        for (int i = 0; i < 256; i++) {
            lut[i] = ((uint32_t)sys_pal[i].peRed << 16) |
                     ((uint32_t)sys_pal[i].peGreen << 8) |
                     ((uint32_t)sys_pal[i].peBlue);
        }
        uint32_t *expanded32 = (uint32_t *)malloc(total_pixels * sizeof(uint32_t));
        int exp_black = 0, exp_nonzero = 0;
        for (int i = 0; i < total_pixels; i++) {
            uint32_t color = lut[pix8[i]];
            expanded32[i] = color;
            if (color == 0) exp_black++;
            else exp_nonzero++;
        }
        printf("Expanded pixels: black=%d (%.1f%%), nonzero=%d (%.1f%%)\n",
               exp_black, (double)exp_black * 100.0 / total_pixels,
               exp_nonzero, (double)exp_nonzero * 100.0 / total_pixels);

        save_bmp32("C:\\probe\\out\\probe_t2_expanded8.bmp", screen_w, screen_h, expanded32);
        printf("Saved probe_t2_expanded8.bmp\n");
        free(expanded32);

        SelectObject(hdc_mem8, old8);
        DeleteObject(hbm8);
        DeleteDC(hdc_mem8);
    }

    // ── Test 3: Select and realize HPALETTE into hdc_screen before 32-bit BitBlt ──
    {
        printf("\n--- Test 3: HPALETTE realization into hdc_screen + 32-bit BitBlt ---\n");
        struct {
            WORD palVersion;
            WORD palNumEntries;
            PALETTEENTRY palPalEntry[256];
        } logpal;
        logpal.palVersion = 0x0300;
        logpal.palNumEntries = 256;
        for (int i = 0; i < 256; i++) {
            logpal.palPalEntry[i] = sys_pal[i];
            logpal.palPalEntry[i].peFlags = PC_RESERVED;
        }

        HPALETTE hpal = CreatePalette((LOGPALETTE *)&logpal);
        printf("CreatePalette result: %p\n", hpal);

        HPALETTE old_pal = SelectPalette(hdc_screen, hpal, FALSE);
        UINT realized = RealizePalette(hdc_screen);
        printf("RealizePalette(hdc_screen) returned: %u\n", realized);

        HDC hdc_mem32 = CreateCompatibleDC(hdc_screen);
        BITMAPINFO bmi32;
        memset(&bmi32, 0, sizeof(bmi32));
        bmi32.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi32.bmiHeader.biWidth = screen_w;
        bmi32.bmiHeader.biHeight = -screen_h;
        bmi32.bmiHeader.biPlanes = 1;
        bmi32.bmiHeader.biBitCount = 32;
        bmi32.bmiHeader.biCompression = BI_RGB;

        uint32_t *pix32 = NULL;
        HBITMAP hbm32 = CreateDIBSection(hdc_screen, &bmi32, DIB_RGB_COLORS, (void **)&pix32, NULL, 0);
        HBITMAP old32 = (HBITMAP)SelectObject(hdc_mem32, hbm32);

        BOOL blt_ok = BitBlt(hdc_mem32, 0, 0, screen_w, screen_h, hdc_screen, 0, 0, SRCCOPY);
        printf("BitBlt result: %s (err=%lu)\n", blt_ok ? "OK" : "FAIL", GetLastError());

        int total_pixels = screen_w * screen_h;
        int black_count = 0, nonzero_count = 0;
        for (int i = 0; i < total_pixels; i++) {
            if ((pix32[i] & 0x00FFFFFF) == 0) black_count++;
            else nonzero_count++;
        }
        printf("Pixels: total=%d, black=%d (%.1f%%), nonzero=%d (%.1f%%)\n",
               total_pixels, black_count, (double)black_count * 100.0 / total_pixels,
               nonzero_count, (double)nonzero_count * 100.0 / total_pixels);

        save_bmp32("C:\\probe\\out\\probe_t3_realized32.bmp", screen_w, screen_h, pix32);
        printf("Saved probe_t3_realized32.bmp\n");

        SelectObject(hdc_mem32, old32);
        DeleteObject(hbm32);
        DeleteDC(hdc_mem32);

        SelectPalette(hdc_screen, old_pal, FALSE);
        DeleteObject(hpal);
    }

    ReleaseDC(NULL, hdc_screen);
    printf("=== Probe Complete ===\n");
    return 0;
}
