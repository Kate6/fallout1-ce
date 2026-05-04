#include "plib/gnw/svga.h"

#ifdef __ANDROID__
#include <android/log.h>
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "FALLOUT", __VA_ARGS__)
#else
#define LOGD(...) printf(__VA_ARGS__)
#endif

#include "plib/gnw/debug.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/winmain.h"

namespace fallout {

static bool createRenderer(int width, int height);
static void destroyRenderer();

// screen rect (internal game resolution)
Rect scr_size;

// 0x6ACA18
ScreenBlitFunc* scr_blit = GNW95_ShowRect;

SDL_Window* gSdlWindow = NULL;
SDL_Surface* gSdlSurface = NULL;
SDL_Renderer* gSdlRenderer = NULL;
SDL_Texture* gSdlTexture = NULL;
SDL_Surface* gSdlTextureSurface = NULL;

// Display surface (full screen size on Android)
static SDL_Surface* gDisplaySurface = NULL;

// Display dimensions
static int gDisplayWidth = 0;
static int gDisplayHeight = 0;

// Native game resolution
#define GAME_NATIVE_WIDTH 640
#define GAME_NATIVE_HEIGHT 480

// TODO: Remove once migration to update-render cycle is completed.
FpsLimiter sharedFpsLimiter;

// Nearest-neighbor scaling from game buffer to display
static void scaleBufferToDisplay(unsigned char* src, int srcW, int srcH,
                                  unsigned char* dst, int dstW, int dstH) {
    float scaleX = (float)srcW / dstW;
    float scaleY = (float)srcH / dstH;

    for (int y = 0; y < dstH; y++) {
        int srcY = (int)(y * scaleY);
        if (srcY >= srcH) srcY = srcH - 1;

        unsigned char* srcRow = src + srcY * srcW;
        unsigned char* dstRow = dst + y * dstW;

        for (int x = 0; x < dstW; x++) {
            int srcX = (int)(x * scaleX);
            if (srcX >= srcW) srcX = srcW - 1;
            dstRow[x] = srcRow[srcX];
        }
    }
}

// 0x4CB310
void GNW95_SetPaletteEntries(unsigned char* palette, int start, int count)
{
    LOGD("GNW95_SetPaletteEntries called: start=%d, count=%d\n", start, count);
    if (gSdlSurface != NULL && gSdlSurface->format->palette != NULL) {
        SDL_Color colors[256];

        if (count != 0) {
            for (int index = 0; index < count; index++) {
                colors[index].r = palette[index * 3] << 2;
                colors[index].g = palette[index * 3 + 1] << 2;
                colors[index].b = palette[index * 3 + 2] << 2;
                colors[index].a = 255;
            }
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, start, count);
        SDL_BlitSurface(gSdlSurface, NULL, gSdlTextureSurface, NULL);
    }

    // Also set palette on display surface
    if (gDisplaySurface != NULL && gDisplaySurface->format->palette != NULL) {
        SDL_Color colors[256];

        if (count != 0) {
            for (int index = 0; index < count; index++) {
                colors[index].r = palette[index * 3] << 2;
                colors[index].g = palette[index * 3 + 1] << 2;
                colors[index].b = palette[index * 3 + 2] << 2;
                colors[index].a = 255;
            }
        }

        SDL_SetPaletteColors(gDisplaySurface->format->palette, colors, start, count);
    }
}

// 0x4CB568
void GNW95_SetPalette(unsigned char* palette)
{
    LOGD("GNW95_SetPalette called\n");
    if (gSdlSurface != NULL && gSdlSurface->format->palette != NULL) {
        SDL_Color colors[256];

        for (int index = 0; index < 256; index++) {
            colors[index].r = palette[index * 3] << 2;
            colors[index].g = palette[index * 3 + 1] << 2;
            colors[index].b = palette[index * 3 + 2] << 2;
            colors[index].a = 255;
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);
        SDL_BlitSurface(gSdlSurface, NULL, gSdlTextureSurface, NULL);
    }

    // Also set palette on display surface
    if (gDisplaySurface != NULL && gDisplaySurface->format->palette != NULL) {
        SDL_Color colors[256];

        for (int index = 0; index < 256; index++) {
            colors[index].r = palette[index * 3] << 2;
            colors[index].g = palette[index * 3 + 1] << 2;
            colors[index].b = palette[index * 3 + 2] << 2;
            colors[index].a = 255;
        }

        SDL_SetPaletteColors(gDisplaySurface->format->palette, colors, 0, 256);
    }
}

// 0x4CB850
// This function is called to blit from internal buffer to display.
void GNW95_ShowRect(unsigned char* src, unsigned int srcPitch, unsigned int a3, unsigned int srcX, unsigned int srcY, unsigned int srcWidth, unsigned int srcHeight, unsigned int destX, unsigned int destY)
{
    LOGD("GNW95_ShowRect: src=%p, dest=(%d,%d), size=%dx%d\n", src, destX, destY, srcWidth, srcHeight);

    // Draw to the main surface (native resolution 640x480)
    buf_to_buf(src + srcPitch * srcY + srcX, srcWidth, srcHeight, srcPitch,
               (unsigned char*)gSdlSurface->pixels + gSdlSurface->pitch * destY + destX,
               gSdlSurface->pitch);

    SDL_Rect srcRect;
    srcRect.x = destX;
    srcRect.y = destY;
    srcRect.w = srcWidth;
    srcRect.h = srcHeight;

    SDL_Rect destRect;
    destRect.x = destX;
    destRect.y = destY;
    SDL_BlitSurface(gSdlSurface, &srcRect, gSdlTextureSurface, &destRect);
}

bool svga_init(VideoOptions* video_options)
{
    LOGD("svga_init called with width=%d, height=%d, fullscreen=%d\n",
                video_options->width, video_options->height, video_options->fullscreen);
    // Set rendering hints BEFORE creating the window and renderer
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
    // Use "none" to stretch to fill the window without preserving aspect ratio
    SDL_SetHint(SDL_HINT_RENDER_LOGICAL_SIZE_MODE, "none");

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        return false;
    }

    Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;

    if (video_options->fullscreen) {
        windowFlags |= SDL_WINDOW_FULLSCREEN;
    }

    // Determine window size
    int windowW = video_options->width * video_options->scale;
    int windowH = video_options->height * video_options->scale;

    // Display dimensions (will be full screen on Android)
    int displayW = video_options->width;
    int displayH = video_options->height;

#if __ANDROID__
    if (video_options->fullscreen) {
        SDL_DisplayMode mode;
        if (SDL_GetDesktopDisplayMode(0, &mode) == 0) {
            windowW = mode.w;
            windowH = mode.h;
            displayW = mode.w;
            displayH = mode.h;
            LOGD("Android fullscreen: using display size %dx%d\n", displayW, displayH);
        } else {
            windowW = 0;
            windowH = 0;
        }
    }
#endif

    gDisplayWidth = displayW;
    gDisplayHeight = displayH;

    gSdlWindow = SDL_CreateWindow(GNW95_title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        windowW, windowH, windowFlags);
    if (gSdlWindow == NULL) {
        return false;
    }

    // Create renderer at display resolution for scaling
    if (!createRenderer(displayW, displayH)) {
        destroyRenderer();

        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;

        return false;
    }

    // Create main surface at NATIVE game resolution (640x480) - this is what the game draws to
    gSdlSurface = SDL_CreateRGBSurface(0,
        GAME_NATIVE_WIDTH,
        GAME_NATIVE_HEIGHT,
        8,
        0,
        0,
        0,
        0);
    if (gSdlSurface == NULL) {
        LOGD("Failed to create gSdlSurface\n");
        destroyRenderer();

        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
    } else {
        LOGD("Successfully created gSdlSurface: %p, %dx%d, 8-bit\n",
                    gSdlSurface, GAME_NATIVE_WIDTH, GAME_NATIVE_HEIGHT);
    }

    // Create display surface at full screen size (for scaling on Android)
    gDisplaySurface = SDL_CreateRGBSurface(0,
        displayW,
        displayH,
        8,
        0,
        0,
        0,
        0);
    if (gDisplaySurface == NULL) {
        LOGD("Failed to create gDisplaySurface\n");
    } else {
        LOGD("Successfully created gDisplaySurface: %p, %dx%d, 8-bit\n",
                    gDisplaySurface, displayW, displayH);
    }

    SDL_Color colors[256];
    for (int index = 0; index < 256; index++) {
        colors[index].r = index;
        colors[index].g = index;
        colors[index].b = index;
        colors[index].a = 255;
    }

    // Set palette for both surfaces
    if (gSdlSurface != NULL) {
        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);
    }
    if (gDisplaySurface != NULL) {
        SDL_SetPaletteColors(gDisplaySurface->format->palette, colors, 0, 256);
    }

    // scr_size is used by the game internally - keep at native resolution
    scr_size.ulx = 0;
    scr_size.uly = 0;
    scr_size.lrx = GAME_NATIVE_WIDTH - 1;
    scr_size.lry = GAME_NATIVE_HEIGHT - 1;
    LOGD("Internal game screen size set to %dx%d\n", GAME_NATIVE_WIDTH, GAME_NATIVE_HEIGHT);

    mouse_blit_trans = NULL;
    scr_blit = GNW95_ShowRect;
    mouse_blit = GNW95_ShowRect;

    return true;
}

void svga_exit()
{
    if (gDisplaySurface != NULL) {
        SDL_FreeSurface(gDisplaySurface);
        gDisplaySurface = NULL;
    }

    if (gSdlSurface != NULL) {
        SDL_FreeSurface(gSdlSurface);
        gSdlSurface = NULL;
    }

    destroyRenderer();

    if (gSdlWindow != NULL) {
        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int screenGetWidth()
{
    // Return the internal game resolution
    return GAME_NATIVE_WIDTH;
}

int screenGetHeight()
{
    // Return the internal game resolution
    return GAME_NATIVE_HEIGHT;
}

int getDisplayWidth()
{
    return gDisplayWidth;
}

int getDisplayHeight()
{
    return gDisplayHeight;
}

static bool createRenderer(int width, int height)
{
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, 0);
    if (gSdlRenderer == NULL) {
        return false;
    }

    if (SDL_RenderSetLogicalSize(gSdlRenderer, width, height) != 0) {
        return false;
    }

    gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (gSdlTexture == NULL) {
        return false;
    }

    // Set texture scale mode to linear for better quality during scaling
    SDL_SetTextureScaleMode(gSdlTexture, SDL_ScaleModeLinear);

    Uint32 format;
    if (SDL_QueryTexture(gSdlTexture, &format, NULL, NULL, NULL) != 0) {
        return false;
    }

    gSdlTextureSurface = SDL_CreateRGBSurfaceWithFormat(0, width, height, SDL_BITSPERPIXEL(format), format);
    if (gSdlTextureSurface == NULL) {
        LOGD("Failed to create gSdlTextureSurface\n");
        return false;
    } else {
        LOGD("Successfully created gSdlTextureSurface: %p, %dx%d, format=0x%x\n",
                    gSdlTextureSurface, width, height, format);
    }

    return true;
}

static void destroyRenderer()
{
    if (gSdlTextureSurface != NULL) {
        SDL_FreeSurface(gSdlTextureSurface);
        gSdlTextureSurface = NULL;
    }

    if (gSdlTexture != NULL) {
        SDL_DestroyTexture(gSdlTexture);
        gSdlTexture = NULL;
    }

    if (gSdlRenderer != NULL) {
        SDL_DestroyRenderer(gSdlRenderer);
        gSdlRenderer = NULL;
    }
}

void handleWindowSizeChanged()
{
    destroyRenderer();
    createRenderer(gDisplayWidth, gDisplayHeight);
}

void setLogicalSize(int width, int height)
{
    SDL_RenderSetLogicalSize(gSdlRenderer, width, height);
}

void renderPresent()
{
    if (gSdlRenderer == NULL || gSdlTexture == NULL || gSdlTextureSurface == NULL) {
        LOGD("renderPresent: Missing required objects\n");
        return;
    }

#if __ANDROID__
    // On Android, scale game surface to display surface
    if (gDisplaySurface != NULL && gSdlSurface != NULL && gDisplayWidth > GAME_NATIVE_WIDTH) {
        // Fill display surface with black
        SDL_FillRect(gDisplaySurface, NULL, SDL_MapRGB(gDisplaySurface->format, 0, 0, 0));

        // Scale game buffer to display surface
        scaleBufferToDisplay(
            (unsigned char*)gSdlSurface->pixels,
            gSdlSurface->w,
            gSdlSurface->h,
            (unsigned char*)gDisplaySurface->pixels,
            gDisplaySurface->w,
            gDisplaySurface->h);

        // Copy display surface to texture surface
        SDL_Rect fullDest = {0, 0, gDisplaySurface->w, gDisplaySurface->h};
        SDL_BlitSurface(gDisplaySurface, NULL, gSdlTextureSurface, &fullDest);
    } else {
        // Non-Android path - copy game surface directly
        SDL_FillRect(gSdlTextureSurface, NULL, SDL_MapRGB(gSdlTextureSurface->format, 0, 0, 0));
        SDL_Rect fullDest = {0, 0, gSdlSurface->w, gSdlSurface->h};
        SDL_BlitSurface(gSdlSurface, NULL, gSdlTextureSurface, &fullDest);
    }
#else
    // Non-Android path
    SDL_FillRect(gSdlTextureSurface, NULL, SDL_MapRGB(gSdlTextureSurface->format, 0, 0, 0));
    SDL_Rect fullDest = {0, 0, gSdlSurface->w, gSdlSurface->h};
    SDL_BlitSurface(gSdlSurface, NULL, gSdlTextureSurface, &fullDest);
#endif

    SDL_UpdateTexture(gSdlTexture, NULL, gSdlTextureSurface->pixels, gSdlTextureSurface->pitch);
    SDL_RenderClear(gSdlRenderer);
    SDL_RenderCopy(gSdlRenderer, gSdlTexture, NULL, NULL);
    SDL_RenderPresent(gSdlRenderer);
}

// Get the display surface for movie code to write to
SDL_Surface* getDisplaySurface()
{
    return gDisplaySurface;
}

} // namespace fallout