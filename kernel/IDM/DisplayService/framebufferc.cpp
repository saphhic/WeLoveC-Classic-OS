// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements text and graphics drawing on the framebuffer.

//written by saphhic.

#include "framebuffer.hpp"
#include "gpu.hpp"
#include "IntegratedFontService/font16x16_basic.h"
#include "IntegratedFontService/font8x8_basic.h"
#include "IntegratedFontService/font_ubuntu_bitmap_16x16.h"
#include "arch/common/MM/Heap/heap.hpp"
#include "arch/common/MM/PMM/pmm.hpp"
#include "arch/x86_64/mm/physical/physical_memory.hpp"
#include "arch/x86_64/mm/spinlock.hpp"

uintptr_t FRAMEBUFFER_ADDR = 0;
uint32_t FRAMEBUFFER_WIDTH = 0;
uint32_t FRAMEBUFFER_HEIGHT = 0;
uint32_t FRAMEBUFFER_PITCH = 0;

uint32_t* framebuffer = nullptr;
uint32_t width = 0;
uint32_t height = 0;
uint32_t pitch = 0;

static int fb_cursor_x = 0;
static int fb_cursor_y = 0;
static int fb_char_scale = 1;
static uint32_t fb_char_color = 0xFFFFFFFF;
static uint32_t fb_bg_color = 0x00000000;
static const int FB_MARGIN = 8;
static uint32_t fb_pixel_format = UINT32_MAX;
static uint32_t fb_pixel_masks[4]{};
static const int FB_FONT_WIDTH = 16;
static const int FB_FONT_HEIGHT = 16;
static uint8_t *gScrollbackRows;
static uint8_t *gScrollbackLiveSnapshot;
static uint32_t gScrollbackRowCapacity;
static uint32_t gScrollbackRowCount;
static uint32_t gScrollbackWriteRow;
static uint32_t gScrollbackViewOffset;
static uint32_t gScrollbackWidth;
static uint32_t gScrollbackHeight;
static uint32_t gScrollbackPitch;
static welovec::x86_64::mm::IrqSpinLock gSurfaceLock;
static uint64_t gSurfacePixelsAllocated;
static bool gVirtualFramebufferActive;
static constexpr uint64_t kMaximumWindowSurfacePixels = UINT64_C(4096) * 2160;
static constexpr uint64_t kMaximumTotalWindowSurfacePixels = UINT64_C(64) * 1024 * 1024;
static welovec::x86_64::mm::IrqSpinLock gFrameLock;
static uint32_t *gFrameBuffer;
static bool gFrameBufferAllocationFailed;
static uint64_t gFrameOwner;
static int32_t gFrameX, gFrameY;
static uint32_t gFrameWidth, gFrameHeight;
static bool gFramePreparing;
static bool gFrameCancelRequested;
static bool gFrameGpuPresentCandidate;
static bool gFrameGpuPresentationFailed;

static bool framebuffer_gpu_surface_format(uint32_t *out_format) {
    /* UEFI BGRR memory matches XRGB8888 on little-endian x86. Other GOP
       layouts need conversion before they can be passed to a native driver. */
    if (!out_format || fb_pixel_format != 1) return false;
    *out_format = WELOVEC_GPU_PIXEL_XRGB8888;
    return true;
}

static void copy_forward_bytes(uint8_t *destination, const uint8_t *source,
                               uint64_t bytes) {
    if (!destination || !source || !bytes) return;
    uint64_t words = bytes / sizeof(uint64_t);
    const uint64_t tail = bytes % sizeof(uint64_t);
    if (words) {
        __asm__ volatile("cld; rep movsq"
                         : "+D"(destination), "+S"(source), "+c"(words)
                         : : "memory");
    }
    for (uint64_t i = 0; i < tail; ++i) destination[i] = source[i];
}

static void fill_framebuffer_pixels(uint32_t *destination, uint32_t pixel,
                                    uint32_t count) {
    if (!destination || !count) return;
    __asm__ volatile("cld; rep stosl"
                     : "+D"(destination), "+c"(count)
                     : "a"(pixel) : "memory");
}

static bool surface_backing_available(uint64_t pixels) {
    if (pixels > UINT64_MAX / sizeof(uint32_t)) return false;
    const auto memory = welovec::memory::pmm::statistics();
    if (memory.free_frames > UINT64_MAX / 4096) return true;
    const uint64_t free_bytes = memory.free_frames * 4096;
    /* Window surfaces may consume at most one quarter of current free RAM,
       leaving room for process stacks, page tables, and kernel allocations. */
    return pixels * sizeof(uint32_t) <= free_bytes / 4;
}

static bool surface_dimensions_valid(uint32_t surface_width,
                                     uint32_t surface_height) {
    return surface_width && surface_height && surface_width <= 4096 &&
           surface_height <= 2160 &&
           static_cast<uint64_t>(surface_width) * surface_height <=
               kMaximumWindowSurfacePixels;
}

bool framebuffer_surface_create(WelovecFramebufferSurface *surface,
                                uint32_t surface_width,
                                uint32_t surface_height) {
    if (!surface || surface->pixels ||
        !surface_dimensions_valid(surface_width, surface_height)) return false;
    const uint64_t pixel_count =
        static_cast<uint64_t>(surface_width) * surface_height;
    if (!surface_backing_available(pixel_count)) return false;
    const uint64_t irq = gSurfaceLock.lock_irqsave();
    if (pixel_count > kMaximumTotalWindowSurfacePixels - gSurfacePixelsAllocated) {
        gSurfaceLock.unlock_irqrestore(irq);
        return false;
    }
    gSurfacePixelsAllocated += pixel_count;
    gSurfaceLock.unlock_irqrestore(irq);

    auto *pixels = static_cast<uint32_t *>(
        welovec::memory::heap::allocate_zeroed(pixel_count * sizeof(uint32_t)));
    if (!pixels) {
        const uint64_t rollback_irq = gSurfaceLock.lock_irqsave();
        gSurfacePixelsAllocated -= pixel_count;
        gSurfaceLock.unlock_irqrestore(rollback_irq);
        return false;
    }
    surface->pixels = pixels;
    surface->width = surface_width;
    surface->height = surface_height;
    surface->pitch = surface_width * sizeof(uint32_t);
    surface->cursor_x = FB_MARGIN;
    surface->cursor_y = FB_MARGIN;
    surface->scale = 1;
    surface->foreground = UINT32_C(0xffffffff);
    surface->background = UINT32_C(0x00000000);
    surface->generation = 0;
    return true;
}

bool framebuffer_surface_resize(WelovecFramebufferSurface *surface,
                                uint32_t surface_width,
                                uint32_t surface_height) {
    if (!surface || !surface->pixels ||
        !surface_dimensions_valid(surface_width, surface_height)) return false;
    if (surface->width == surface_width && surface->height == surface_height)
        return true;

    const uint64_t new_pixels =
        static_cast<uint64_t>(surface_width) * surface_height;
    const uint64_t old_pixel_count =
        static_cast<uint64_t>(surface->width) * surface->height;
    const uint64_t reserve_pixels = new_pixels > old_pixel_count
        ? new_pixels - old_pixel_count : 0;
    if (!surface_backing_available(new_pixels)) return false;
    const uint64_t reserve_irq = gSurfaceLock.lock_irqsave();
    if (reserve_pixels > kMaximumTotalWindowSurfacePixels - gSurfacePixelsAllocated) {
        gSurfaceLock.unlock_irqrestore(reserve_irq);
        return false;
    }
    gSurfacePixelsAllocated += reserve_pixels;
    gSurfaceLock.unlock_irqrestore(reserve_irq);

    auto *replacement = static_cast<uint32_t *>(
        welovec::memory::heap::allocate_zeroed(new_pixels * sizeof(uint32_t)));
    if (!replacement) {
        const uint64_t rollback_irq = gSurfaceLock.lock_irqsave();
        gSurfacePixelsAllocated -= reserve_pixels;
        gSurfaceLock.unlock_irqrestore(rollback_irq);
        return false;
    }

    const uint64_t irq = gSurfaceLock.lock_irqsave();
    const uint32_t copy_width = surface->width < surface_width
        ? surface->width : surface_width;
    const uint32_t copy_height = surface->height < surface_height
        ? surface->height : surface_height;
    const uint64_t copy_bytes = static_cast<uint64_t>(copy_width) * sizeof(uint32_t);
    for (uint32_t row = 0; row < copy_height; ++row) {
        auto *destination = reinterpret_cast<uint8_t *>(replacement) +
            static_cast<uint64_t>(row) * surface_width * sizeof(uint32_t);
        const auto *source = reinterpret_cast<const uint8_t *>(surface->pixels) +
            static_cast<uint64_t>(row) * surface->pitch;
        copy_forward_bytes(destination, source, copy_bytes);
    }

    uint32_t *old_pixels = surface->pixels;
    surface->pixels = replacement;
    surface->width = surface_width;
    surface->height = surface_height;
    surface->pitch = surface_width * sizeof(uint32_t);
    ++surface->generation;
    if (surface->cursor_x >= static_cast<int>(surface_width))
        surface->cursor_x = static_cast<int>(surface_width) - 1;
    if (surface->cursor_y >= static_cast<int>(surface_height))
        surface->cursor_y = static_cast<int>(surface_height) - 1;
    if (new_pixels < old_pixel_count)
        gSurfacePixelsAllocated -= old_pixel_count - new_pixels;
    gSurfaceLock.unlock_irqrestore(irq);
    welovec::memory::heap::deallocate(old_pixels);
    return true;
}

void framebuffer_surface_destroy(WelovecFramebufferSurface *surface) {
    if (!surface || !surface->pixels) return;
    const uint64_t irq = gSurfaceLock.lock_irqsave();
    uint32_t *pixels = surface->pixels;
    const uint64_t count = static_cast<uint64_t>(surface->width) * surface->height;
    surface->pixels = nullptr;
    surface->width = surface->height = surface->pitch = 0;
    surface->generation = 0;
    if (count <= gSurfacePixelsAllocated) gSurfacePixelsAllocated -= count;
    gSurfaceLock.unlock_irqrestore(irq);
    welovec::memory::heap::deallocate(pixels);
}

bool framebuffer_surface_begin(WelovecFramebufferSurface *surface,
                               WelovecFramebufferTargetState *state) {
    if (!surface || !surface->pixels || !state || state->active) return false;
    state->lock_state = gSurfaceLock.lock_irqsave();
    state->surface = surface;
    state->framebuffer = framebuffer;
    state->width = width;
    state->height = height;
    state->pitch = pitch;
    state->cursor_x = fb_cursor_x;
    state->cursor_y = fb_cursor_y;
    state->scale = fb_char_scale;
    state->foreground = fb_char_color;
    state->background = fb_bg_color;
    state->active = true;

    framebuffer = surface->pixels;
    width = surface->width;
    height = surface->height;
    pitch = surface->pitch;
    fb_cursor_x = surface->cursor_x;
    fb_cursor_y = surface->cursor_y;
    fb_char_scale = surface->scale;
    fb_char_color = surface->foreground;
    fb_bg_color = surface->background;
    gVirtualFramebufferActive = true;
    return true;
}

void framebuffer_surface_end(WelovecFramebufferTargetState *state,
                             bool content_changed) {
    if (!state || !state->active) return;
    if (state->surface) {
        state->surface->cursor_x = fb_cursor_x;
        state->surface->cursor_y = fb_cursor_y;
        state->surface->scale = fb_char_scale;
        state->surface->foreground = fb_char_color;
        state->surface->background = fb_bg_color;
        if (content_changed) ++state->surface->generation;
    }
    framebuffer = state->framebuffer;
    width = state->width;
    height = state->height;
    pitch = state->pitch;
    fb_cursor_x = state->cursor_x;
    fb_cursor_y = state->cursor_y;
    fb_char_scale = state->scale;
    fb_char_color = state->foreground;
    fb_bg_color = state->background;
    gVirtualFramebufferActive = false;
    state->active = false;
    gSurfaceLock.unlock_irqrestore(state->lock_state);
}

bool framebuffer_surface_blit(const WelovecFramebufferSurface *surface,
                              int32_t dest_x, int32_t dest_y,
                              int32_t source_x, int32_t source_y,
                              int32_t blit_width, int32_t blit_height) {
    if (!surface || !surface->pixels || !framebuffer_available() ||
        source_x < 0 || source_y < 0 || blit_width <= 0 || blit_height <= 0)
        return false;
    const uint64_t irq = gSurfaceLock.lock_irqsave();
    const int64_t sx0 = source_x;
    const int64_t sy0 = source_y;
    const int64_t sx1 = sx0 + blit_width < surface->width
        ? sx0 + blit_width : surface->width;
    const int64_t sy1 = sy0 + blit_height < surface->height
        ? sy0 + blit_height : surface->height;
    int64_t left = sx0, top = sy0, right = sx1, bottom = sy1;
    if (dest_x < 0) left += -static_cast<int64_t>(dest_x);
    if (dest_y < 0) top += -static_cast<int64_t>(dest_y);
    if (dest_x + (right - sx0) > width) right -= dest_x + (right - sx0) - width;
    if (dest_y + (bottom - sy0) > height) bottom -= dest_y + (bottom - sy0) - height;
    if (right > surface->width) right = surface->width;
    if (bottom > surface->height) bottom = surface->height;
    if (left < sx0) left = sx0;
    if (top < sy0) top = sy0;
    if (left >= right || top >= bottom) {
        gSurfaceLock.unlock_irqrestore(irq);
        return true;
    }
    const int64_t output_x = static_cast<int64_t>(dest_x) + left - sx0;
    const int64_t output_y = static_cast<int64_t>(dest_y) + top - sy0;
    const uint64_t copy_bytes = static_cast<uint64_t>(right - left) * sizeof(uint32_t);
    for (int64_t row = top; row < bottom; ++row) {
        const auto *source = reinterpret_cast<const uint8_t *>(surface->pixels) +
            static_cast<uint64_t>(row) * surface->pitch +
            static_cast<uint64_t>(left) * sizeof(uint32_t);
        auto *destination = reinterpret_cast<uint8_t *>(framebuffer) +
            static_cast<uint64_t>(output_y + row - top) * pitch +
            static_cast<uint64_t>(output_x) * sizeof(uint32_t);
        copy_forward_bytes(destination, source, copy_bytes);
    }
    gSurfaceLock.unlock_irqrestore(irq);
    return true;
}

uint64_t framebuffer_surface_generation(
    const WelovecFramebufferSurface *surface) {
    if (!surface || !surface->pixels) return 0;
    const uint64_t irq = gSurfaceLock.lock_irqsave();
    const uint64_t generation = surface->generation;
    gSurfaceLock.unlock_irqrestore(irq);
    return generation;
}

static void copy_framebuffer_region(uint32_t *destination,
                                    const uint32_t *source,
                                    int32_t x, int32_t y,
                                    uint32_t region_width,
                                    uint32_t region_height) {
    if (!destination || !source || !region_width || !region_height) return;
    const uint64_t row_bytes = static_cast<uint64_t>(region_width) *
                              sizeof(uint32_t);
    for (uint32_t row = 0; row < region_height; ++row) {
        auto *out = reinterpret_cast<uint8_t *>(destination) +
            static_cast<uint64_t>(y + static_cast<int32_t>(row)) * pitch +
            static_cast<uint64_t>(x) * sizeof(uint32_t);
        const auto *in = reinterpret_cast<const uint8_t *>(source) +
            static_cast<uint64_t>(y + static_cast<int32_t>(row)) * pitch +
            static_cast<uint64_t>(x) * sizeof(uint32_t);
        copy_forward_bytes(out, in, row_bytes);
    }
}

bool framebuffer_frame_begin(uint64_t owner, int32_t x, int32_t y,
                             int32_t frame_width, int32_t frame_height) {
    if (!owner || !framebuffer_available() || frame_width <= 0 ||
        frame_height <= 0)
        return false;
    const int64_t right = static_cast<int64_t>(x) + frame_width;
    const int64_t bottom = static_cast<int64_t>(y) + frame_height;
    const int x0 = x < 0 ? 0 : x;
    const int y0 = y < 0 ? 0 : y;
    const int x1 = right > width ? static_cast<int>(width)
                                 : static_cast<int>(right);
    const int y1 = bottom > height ? static_cast<int>(height)
                                   : static_cast<int>(bottom);
    if (x0 >= x1 || y0 >= y1) return false;

    uint32_t gpu_pixel_format = 0;
    const bool gpu_present_candidate =
        framebuffer_gpu_surface_format(&gpu_pixel_format) &&
        !__atomic_load_n(&gFrameGpuPresentationFailed, __ATOMIC_ACQUIRE) &&
        welovec_gpu_can_present(width, height, gpu_pixel_format);

    if (!gFrameBuffer) {
        if (gFrameBufferAllocationFailed) return false;
        const uint64_t buffer_bytes = static_cast<uint64_t>(pitch) * height;
        const uint64_t buffer_pixels = buffer_bytes / sizeof(uint32_t);
        if (!buffer_bytes || (buffer_bytes % sizeof(uint32_t)) != 0 ||
            buffer_pixels > kMaximumWindowSurfacePixels ||
            !surface_backing_available(buffer_pixels)) {
            gFrameBufferAllocationFailed = true;
            return false;
        }
        auto *candidate = static_cast<uint32_t *>(
            welovec::memory::heap::allocate(buffer_bytes));
        if (!candidate) {
            gFrameBufferAllocationFailed = true;
            return false;
        }

        const uint64_t allocation_irq = gFrameLock.lock_irqsave();
        const bool discard_candidate = gFrameBuffer != nullptr;
        if (!discard_candidate) gFrameBuffer = candidate;
        gFrameLock.unlock_irqrestore(allocation_irq);
        if (discard_candidate)
            (void)welovec::memory::heap::deallocate(candidate);
    }

    uint64_t irq = gFrameLock.lock_irqsave();
    if (__atomic_load_n(&gFrameOwner, __ATOMIC_ACQUIRE)) {
        gFrameLock.unlock_irqrestore(irq);
        return false;
    }
    gFrameX = x0;
    gFrameY = y0;
    gFrameWidth = static_cast<uint32_t>(x1 - x0);
    gFrameHeight = static_cast<uint32_t>(y1 - y0);
    __atomic_store_n(&gFrameOwner, owner, __ATOMIC_RELEASE);
    __atomic_store_n(&gFramePreparing, true, __ATOMIC_RELEASE);
    gFrameCancelRequested = false;
    gFrameGpuPresentCandidate = gpu_present_candidate;
    gFrameLock.unlock_irqrestore(irq);

    if (gpu_present_candidate) {
        /* Native present consumes a complete frame, so synchronize its
           backing surface before applying this transaction's dirty region. */
        copy_framebuffer_region(gFrameBuffer, framebuffer, 0, 0, width, height);
    } else {
        copy_framebuffer_region(gFrameBuffer, framebuffer, x0, y0,
                                static_cast<uint32_t>(x1 - x0),
                                static_cast<uint32_t>(y1 - y0));
    }

    irq = gFrameLock.lock_irqsave();
    const bool canceled =
        __atomic_load_n(&gFrameOwner, __ATOMIC_ACQUIRE) != owner ||
        gFrameCancelRequested;
    __atomic_store_n(&gFramePreparing, false, __ATOMIC_RELEASE);
    if (canceled &&
        __atomic_load_n(&gFrameOwner, __ATOMIC_ACQUIRE) == owner) {
        __atomic_store_n(&gFrameOwner, UINT64_C(0), __ATOMIC_RELEASE);
        gFrameX = gFrameY = 0;
        gFrameWidth = gFrameHeight = 0;
        gFrameGpuPresentCandidate = false;
        gFrameCancelRequested = false;
    }
    gFrameLock.unlock_irqrestore(irq);
    return !canceled;
}

bool framebuffer_frame_end(uint64_t owner) {
    if (!owner) return false;
    uint64_t irq = gFrameLock.lock_irqsave();
    if (__atomic_load_n(&gFrameOwner, __ATOMIC_ACQUIRE) != owner ||
        !gFrameBuffer ||
        __atomic_load_n(&gFramePreparing, __ATOMIC_ACQUIRE)) {
        gFrameLock.unlock_irqrestore(irq);
        return false;
    }

    __atomic_store_n(&gFramePreparing, true, __ATOMIC_RELEASE);
    const bool try_gpu_present = gFrameGpuPresentCandidate;
    uint32_t gpu_pixel_format = 0;
    WelovecGpuSurface gpu_surface{};
    if (try_gpu_present &&
        framebuffer_gpu_surface_format(&gpu_pixel_format)) {
        gpu_surface.pixels = gFrameBuffer;
        gpu_surface.size_bytes = static_cast<uint64_t>(pitch) * height;
        gpu_surface.width = width;
        gpu_surface.height = height;
        gpu_surface.pitch_bytes = pitch;
        gpu_surface.pixel_format = gpu_pixel_format;
    }
    gFrameLock.unlock_irqrestore(irq);

    /* The callback runs without the IRQ spinlock held. A driver can wait for
       hardware completion here; on any error the GOP copy below still wins. */
    const WelovecGpuStatus gpu_status =
        try_gpu_present && gpu_surface.pixels
            ? welovec_gpu_present_active(&gpu_surface)
            : WELOVEC_GPU_UNSUPPORTED;

    irq = gFrameLock.lock_irqsave();
    if (__atomic_load_n(&gFrameOwner, __ATOMIC_ACQUIRE) != owner ||
        !gFrameBuffer) {
        gFrameLock.unlock_irqrestore(irq);
        return false;
    }
    if (try_gpu_present && gpu_status != WELOVEC_GPU_OK)
        __atomic_store_n(&gFrameGpuPresentationFailed, true, __ATOMIC_RELEASE);

    /* Keep the firmware framebuffer current as the immediate CPU fallback. */
    copy_framebuffer_region(framebuffer, gFrameBuffer, gFrameX, gFrameY,
                            gFrameWidth, gFrameHeight);
    /* GOP framebuffer pages are mapped write-combining; drain WC stores before
       the transaction is released or the display can observe stale chunks. */
    __asm__ volatile("sfence" ::: "memory");
    __atomic_store_n(&gFrameOwner, UINT64_C(0), __ATOMIC_RELEASE);
    gFrameX = gFrameY = 0;
    gFrameWidth = gFrameHeight = 0;
    __atomic_store_n(&gFramePreparing, false, __ATOMIC_RELEASE);
    gFrameCancelRequested = false;
    gFrameGpuPresentCandidate = false;
    gFrameLock.unlock_irqrestore(irq);
    return true;
}

void framebuffer_frame_cancel_owner(uint64_t owner) {
    if (!owner) return;
    const uint64_t irq = gFrameLock.lock_irqsave();
    if (__atomic_load_n(&gFrameOwner, __ATOMIC_ACQUIRE) == owner) {
        if (__atomic_load_n(&gFramePreparing, __ATOMIC_ACQUIRE)) {
            gFrameCancelRequested = true;
        } else {
            __atomic_store_n(&gFrameOwner, UINT64_C(0), __ATOMIC_RELEASE);
            gFrameX = gFrameY = 0;
            gFrameWidth = gFrameHeight = 0;
            gFrameGpuPresentCandidate = false;
            gFrameCancelRequested = false;
        }
    }
    gFrameLock.unlock_irqrestore(irq);
}

bool framebuffer_frame_redirect_begin(
    uint64_t owner, WelovecFramebufferRedirectState *state) {
    if (!state) return false;
    state->active = false;
    for (;;) {
        uint64_t flags;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
        if (!owner ||
            __atomic_load_n(&gFrameOwner, __ATOMIC_ACQUIRE) != owner ||
            !gFrameBuffer) {
            if (flags & (UINT64_C(1) << 9))
                __asm__ volatile("sti" ::: "memory");
            return false;
        }
        if (!__atomic_load_n(&gFramePreparing, __ATOMIC_ACQUIRE)) {
            state->framebuffer = framebuffer;
            state->interrupt_flags = flags;
            state->active = true;
            framebuffer = gFrameBuffer;
            return true;
        }
        if (flags & (UINT64_C(1) << 9))
            __asm__ volatile("sti" ::: "memory");
        __asm__ volatile("pause" ::: "memory");
    }
}

void framebuffer_frame_redirect_end(WelovecFramebufferRedirectState *state) {
    if (!state || !state->active) return;
    framebuffer = state->framebuffer;
    state->active = false;
    if (state->interrupt_flags & (UINT64_C(1) << 9))
        __asm__ volatile("sti" ::: "memory");
}

static uint64_t framebuffer_row_bytes(void) {
    return static_cast<uint64_t>(width) * sizeof(uint32_t);
}

static bool ensure_scrollback_storage(void) {
    if (gScrollbackRows && gScrollbackWidth == width &&
        gScrollbackHeight == height && gScrollbackPitch == pitch)
        return true;
    if (gScrollbackLiveSnapshot) {
        (void)welovec::memory::heap::deallocate(gScrollbackLiveSnapshot);
        gScrollbackLiveSnapshot = nullptr;
    }
    if (gScrollbackRows) {
        (void)welovec::memory::heap::deallocate(gScrollbackRows);
        gScrollbackRows = nullptr;
    }
    gScrollbackRowCapacity = 0;
    gScrollbackRowCount = 0;
    gScrollbackWriteRow = 0;
    gScrollbackViewOffset = 0;
    const uint64_t row_bytes = framebuffer_row_bytes();
    if (!row_bytes || row_bytes > UINT64_C(16) * 1024 * 1024) return false;
    uint64_t capacity = (UINT64_C(16) * 1024 * 1024) / row_bytes;
    if (capacity > 512) capacity = 512;
    if (!capacity) return false;
    gScrollbackRows = static_cast<uint8_t *>(
        welovec::memory::heap::allocate(row_bytes * capacity));
    if (!gScrollbackRows) return false;
    gScrollbackRowCapacity = static_cast<uint32_t>(capacity);
    gScrollbackWidth = width;
    gScrollbackHeight = height;
    gScrollbackPitch = pitch;
    return true;
}

static void capture_scrollback_rows(uint32_t rows) {
    /* Windowed applications have private surfaces and no scrollback view.
       Keep their output from allocating a second global display history. */
    if (gVirtualFramebufferActive) return;
    if (!framebuffer_available() || !ensure_scrollback_storage()) return;
    const uint32_t captured_rows = rows < height ? rows : height;
    const uint64_t row_bytes = framebuffer_row_bytes();
    const uint8_t *source = reinterpret_cast<const uint8_t *>(framebuffer);
    for (uint32_t row = 0; row < captured_rows; ++row) {
        uint8_t *destination = gScrollbackRows +
            static_cast<uint64_t>(gScrollbackWriteRow) * row_bytes;
        const uint8_t *source_row = source + static_cast<uint64_t>(row) * pitch;
        for (uint64_t byte = 0; byte < row_bytes; ++byte)
            destination[byte] = source_row[byte];
        gScrollbackWriteRow = (gScrollbackWriteRow + 1) % gScrollbackRowCapacity;
        if (gScrollbackRowCount < gScrollbackRowCapacity) ++gScrollbackRowCount;
    }
}

void fb_scrollback_return_live(void) {
    if (gVirtualFramebufferActive) return;
    if (!gScrollbackLiveSnapshot) {
        gScrollbackViewOffset = 0;
        return;
    }
    if (framebuffer_available()) {
        uint8_t *destination = reinterpret_cast<uint8_t *>(framebuffer);
        for (uint32_t row = 0; row < height; ++row) {
            const uint8_t *source_row = gScrollbackLiveSnapshot +
                static_cast<uint64_t>(row) * pitch;
            uint8_t *destination_row = destination + static_cast<uint64_t>(row) * pitch;
            for (uint32_t byte = 0; byte < pitch; ++byte)
                destination_row[byte] = source_row[byte];
        }
    }
    (void)welovec::memory::heap::deallocate(gScrollbackLiveSnapshot);
    gScrollbackLiveSnapshot = nullptr;
    gScrollbackViewOffset = 0;
}

static void render_scrollback_view(void) {
    if (!gScrollbackLiveSnapshot || !framebuffer_available()) return;
    const uint64_t row_bytes = framebuffer_row_bytes();
    uint8_t *destination = reinterpret_cast<uint8_t *>(framebuffer);
    for (uint32_t row = 0; row < height; ++row) {
        uint8_t *destination_row = destination + static_cast<uint64_t>(row) * pitch;
        if (row < gScrollbackViewOffset) {
            const uint32_t distance_from_live = gScrollbackViewOffset - row;
            const uint32_t history_index =
                (gScrollbackWriteRow + gScrollbackRowCapacity -
                 distance_from_live % gScrollbackRowCapacity) % gScrollbackRowCapacity;
            const uint8_t *source_row = gScrollbackRows +
                static_cast<uint64_t>(history_index) * row_bytes;
            for (uint64_t byte = 0; byte < row_bytes; ++byte)
                destination_row[byte] = source_row[byte];
        } else {
            const uint8_t *source_row = gScrollbackLiveSnapshot +
                static_cast<uint64_t>(row - gScrollbackViewOffset) * pitch;
            for (uint64_t byte = 0; byte < row_bytes; ++byte)
                destination_row[byte] = source_row[byte];
        }
    }
}

bool fb_scrollback_page(int direction) {
    if (gVirtualFramebufferActive || direction < -1 || direction > 1 ||
        !framebuffer_available()) return false;
    if (!direction) {
        fb_scrollback_return_live();
        return true;
    }
    if (!ensure_scrollback_storage() || !gScrollbackRowCount) return false;

    if (direction > 0) {
        if (!gScrollbackLiveSnapshot) {
            const uint64_t snapshot_size = static_cast<uint64_t>(pitch) * height;
            gScrollbackLiveSnapshot = static_cast<uint8_t *>(
                welovec::memory::heap::allocate(snapshot_size));
            if (!gScrollbackLiveSnapshot) {
                return false;
            }
            const uint8_t *source = reinterpret_cast<const uint8_t *>(framebuffer);
            for (uint32_t row = 0; row < height; ++row) {
                const uint8_t *source_row = source + static_cast<uint64_t>(row) * pitch;
                uint8_t *destination_row = gScrollbackLiveSnapshot +
                    static_cast<uint64_t>(row) * pitch;
                for (uint32_t byte = 0; byte < pitch; ++byte)
                    destination_row[byte] = source_row[byte];
            }
        }
        const uint32_t page_rows = height > FB_FONT_HEIGHT
            ? height - FB_FONT_HEIGHT : height;
        uint32_t target = gScrollbackViewOffset + page_rows;
        if (target < gScrollbackViewOffset || target > gScrollbackRowCount)
            target = gScrollbackRowCount;
        if (target == gScrollbackViewOffset) return false;
        gScrollbackViewOffset = target;
        render_scrollback_view();
        return true;
    }

    const uint32_t page_rows = height > FB_FONT_HEIGHT
        ? height - FB_FONT_HEIGHT : height;
    gScrollbackViewOffset = gScrollbackViewOffset > page_rows
        ? gScrollbackViewOffset - page_rows : 0;
    if (!gScrollbackViewOffset) fb_scrollback_return_live();
    else render_scrollback_view();
    return true;
}

static bool framebuffer_font_metrics(uint32_t font, int *font_width,
                                     int *font_height) {
    if (!font_width || !font_height) return false;
    switch (font) {
        case WELOVEC_FB_FONT_SYSTEM16:
        case WELOVEC_FB_FONT_UBUNTU:
            *font_width = 16;
            *font_height = 16;
            return true;
        case WELOVEC_FB_FONT_SYSTEM8:
            *font_width = 8;
            *font_height = 8;
            return true;
        default:
            return false;
    }
}

bool framebuffer_font_valid(uint32_t font) {
    int font_width = 0, font_height = 0;
    return framebuffer_font_metrics(font, &font_width, &font_height);
}

static uint8_t unpack_channel(uint32_t pixel, uint32_t mask, uint8_t fallback) {
    if (!mask) return fallback;
    uint32_t packed = 0;
    uint32_t output_bit = 0;
    for (uint32_t input_bit = 0; input_bit < 32; ++input_bit) {
        if (!(mask & (UINT32_C(1) << input_bit))) continue;
        if (pixel & (UINT32_C(1) << input_bit)) packed |= UINT32_C(1) << output_bit;
        ++output_bit;
    }
    const uint64_t maximum = output_bit == 32 ? UINT32_MAX : ((UINT64_C(1) << output_bit) - 1);
    return static_cast<uint8_t>((static_cast<uint64_t>(packed) * 255 + maximum / 2) / maximum);
}

static uint32_t pack_channel(uint8_t channel, uint32_t mask) {
    if (!mask) return 0;
    uint32_t output_bit = 0;
    for (uint32_t input_bit = 0; input_bit < 32; ++input_bit)
        if (mask & (UINT32_C(1) << input_bit)) ++output_bit;
    const uint64_t maximum = output_bit == 32 ? UINT32_MAX : ((UINT64_C(1) << output_bit) - 1);
    uint32_t packed = static_cast<uint32_t>((static_cast<uint64_t>(channel) * maximum + 127) / 255);
    uint32_t result = 0;
    uint32_t packed_bit = 0;
    for (uint32_t output = 0; output < 32; ++output) {
        if (!(mask & (UINT32_C(1) << output))) continue;
        if (packed & (UINT32_C(1) << packed_bit)) result |= UINT32_C(1) << output;
        ++packed_bit;
    }
    return result;
}

static uint32_t to_framebuffer_pixel(uint32_t color) {
    if (fb_pixel_format == 0) {
        return (color & UINT32_C(0xff00ff00)) |
               ((color >> 16) & UINT32_C(0x000000ff)) |
               ((color & UINT32_C(0x000000ff)) << 16);
    }
    if (fb_pixel_format == 2) {
        return pack_channel(static_cast<uint8_t>(color >> 16), fb_pixel_masks[0]) |
               pack_channel(static_cast<uint8_t>(color >> 8), fb_pixel_masks[1]) |
               pack_channel(static_cast<uint8_t>(color), fb_pixel_masks[2]) |
               pack_channel(static_cast<uint8_t>(color >> 24), fb_pixel_masks[3]);
    }
    return color;
}

static uint32_t from_framebuffer_pixel(uint32_t pixel) {
    if (fb_pixel_format == 0) {
        return (pixel & UINT32_C(0xff00ff00)) |
               ((pixel >> 16) & UINT32_C(0x000000ff)) |
               ((pixel & UINT32_C(0x000000ff)) << 16);
    }
    if (fb_pixel_format == 2) {
        return (static_cast<uint32_t>(unpack_channel(pixel, fb_pixel_masks[3], 255)) << 24) |
               (static_cast<uint32_t>(unpack_channel(pixel, fb_pixel_masks[0], 0)) << 16) |
               (static_cast<uint32_t>(unpack_channel(pixel, fb_pixel_masks[1], 0)) << 8) |
               unpack_channel(pixel, fb_pixel_masks[2], 0);
    }
    return pixel;
}

uint32_t fb_color(uint8_t r, uint8_t g, uint8_t b) {
    return (0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    
}

bool framebuffer_init(const WelovecBootInfo *boot_info) {
    framebuffer = nullptr;
    width = height = pitch = 0;
    FRAMEBUFFER_ADDR = 0;
    FRAMEBUFFER_WIDTH = FRAMEBUFFER_HEIGHT = FRAMEBUFFER_PITCH = 0;
    fb_pixel_format = UINT32_MAX;
    __atomic_store_n(&gFramePreparing, false, __ATOMIC_RELEASE);
    gFrameCancelRequested = false;
    __atomic_store_n(&gFrameGpuPresentationFailed, false, __ATOMIC_RELEASE);
    gFrameGpuPresentCandidate = false;
    for (uint32_t i = 0; i < 4; ++i) fb_pixel_masks[i] = 0;
    fb_cursor_x = fb_cursor_y = 0;
    if (!boot_info || boot_info->magic != WELOVEC_BOOTINFO_MAGIC ||
        boot_info->version != WELOVEC_BOOTINFO_VERSION ||
        boot_info->size < sizeof(WelovecBootInfo) ||
        !(boot_info->flags & WELOVEC_BOOT_FLAG_GOP_VALID)) return false;

    const WelovecFramebuffer &mode = boot_info->framebuffer;
    const uint64_t direct_map_base = welovec::x86_64::mm::physical::kDirectMapBase;
    const uint64_t direct_map_size = welovec::x86_64::mm::physical::kDirectMapSize;
    if (!mode.base || !mode.size || !mode.width || !mode.height ||
        mode.width > INT32_MAX || mode.height > INT32_MAX ||
        mode.pixels_per_scanline < mode.width || mode.pixel_format > 2 ||
        mode.base >= direct_map_size || mode.size > direct_map_size - mode.base ||
        (mode.base & (alignof(uint32_t) - 1u)) ||
        mode.pixels_per_scanline > UINT32_MAX / sizeof(uint32_t)) {
        return false;
    }

    const uint64_t row_pitch = static_cast<uint64_t>(mode.pixels_per_scanline) * sizeof(uint32_t);
    if (mode.height > UINT64_MAX / row_pitch || row_pitch * mode.height > mode.size) {
        return false;
    }

    FRAMEBUFFER_ADDR = static_cast<uintptr_t>(mode.base);
    FRAMEBUFFER_WIDTH = width = mode.width;
    FRAMEBUFFER_HEIGHT = height = mode.height;
    FRAMEBUFFER_PITCH = pitch = static_cast<uint32_t>(row_pitch);
    fb_pixel_format = mode.pixel_format;
    for (uint32_t i = 0; i < 4; ++i) fb_pixel_masks[i] = mode.pixel_masks[i];
    framebuffer = reinterpret_cast<uint32_t *>(static_cast<uintptr_t>(direct_map_base + mode.base));

    fb_cursor_x = FB_MARGIN;
    fb_cursor_y = FB_MARGIN;
    fb_clear_screen();
    return framebuffer_available();
}

bool framebuffer_available() {
    return framebuffer != nullptr && width > 0 && height > 0 &&
           (uint64_t)pitch >= (uint64_t)width * sizeof(uint32_t);
}

bool framebuffer_get_resolution(uint32_t *out_width, uint32_t *out_height) {
    if (!out_width || !out_height || !framebuffer_available()) return false;
    *out_width = width;
    *out_height = height;
    return true;
}

void fb_set_scale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    fb_char_scale = scale;
}

void fb_set_colors(uint32_t fg, uint32_t bg) {
    fb_char_color = fg;
    fb_bg_color = bg;
}

void put_pixel(int x, int y, uint32_t color) {
    if (framebuffer == nullptr) return;
    if (x < 0 || y < 0 || (uint32_t)x >= width || (uint32_t)y >= height) return;
    uint32_t* pixel = (uint32_t*)((uint8_t*)framebuffer + (uint64_t)(uint32_t)y * pitch + (uint64_t)(uint32_t)x * 4u);
    *pixel = to_framebuffer_pixel(color);
}

static bool clip_rect(int x, int y, int w, int h,
                      int* x0, int* y0, int* x1, int* y1) {
    if (w <= 0 || h <= 0) return false;

    int64_t right = (int64_t)x + w;
    int64_t bottom = (int64_t)y + h;
    *x0 = x < 0 ? 0 : x;
    *y0 = y < 0 ? 0 : y;
    *x1 = right > width ? (int)width : (int)right;
    *y1 = bottom > height ? (int)height : (int)bottom;
    return *x0 < *x1 && *y0 < *y1;
}

void draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (!framebuffer_available()) return;

    int x0, y0, x1, y1;
    if (!clip_rect(x, y, w, h, &x0, &y0, &x1, &y1)) return;

    const uint32_t pixel = to_framebuffer_pixel(color);
    const uint32_t count = static_cast<uint32_t>(x1 - x0);
    for (int row = y0; row < y1; row++) {
        auto *line = reinterpret_cast<uint32_t *>(
            reinterpret_cast<uint8_t *>(framebuffer) +
            static_cast<uint64_t>(static_cast<uint32_t>(row)) * pitch) + x0;
        fill_framebuffer_pixels(line, pixel, count);
    }
}

void fb_scroll_up_region(int x, int y, int w, int h, int rows,
                         uint32_t fill_color) {
    if (!framebuffer_available() || rows <= 0) return;
    fb_scrollback_return_live();

    int x0, y0, x1, y1;
    if (!clip_rect(x, y, w, h, &x0, &y0, &x1, &y1)) return;
    const int clipped_height = y1 - y0;
    if (!gVirtualFramebufferActive && x0 == 0 && y0 == 0 &&
        x1 >= static_cast<int>(width) &&
        y1 >= static_cast<int>(height))
        capture_scrollback_rows(static_cast<uint32_t>(rows));
    if (rows >= clipped_height) {
        draw_rect(x0, y0, x1 - x0, clipped_height, fill_color);
        return;
    }

    for (int row = y0; row < y1 - rows; ++row) {
        uint32_t *destination = reinterpret_cast<uint32_t *>(
            reinterpret_cast<uint8_t *>(framebuffer) +
            static_cast<uint64_t>(static_cast<uint32_t>(row)) * pitch) + x0;
        const uint32_t *source = reinterpret_cast<const uint32_t *>(
            reinterpret_cast<const uint8_t *>(framebuffer) +
            static_cast<uint64_t>(static_cast<uint32_t>(row + rows)) * pitch) + x0;
        for (int column = 0; column < x1 - x0; ++column)
            destination[column] = source[column];
    }

    draw_rect(x0, y1 - rows, x1 - x0, rows, fill_color);
}

static uint32_t blend_pixel(uint32_t dst, uint32_t src, uint8_t alpha) {
    uint32_t inv = 255u - (uint32_t)alpha;
    uint32_t sr = (src >> 16) & 0xFFu;
    uint32_t sg = (src >> 8) & 0xFFu;
    uint32_t sb = src & 0xFFu;
    uint32_t dr = (dst >> 16) & 0xFFu;
    uint32_t dg = (dst >> 8) & 0xFFu;
    uint32_t db = dst & 0xFFu;
    uint32_t r = (sr * alpha + dr * inv) / 255u;
    uint32_t g = (sg * alpha + dg * inv) / 255u;
    uint32_t b = (sb * alpha + db * inv) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void draw_rect_alpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
    if (!framebuffer_available() || alpha == 0) return;
    if (alpha == 255) {
        draw_rect(x, y, w, h, color);
        return;
    }

    int x0, y0, x1, y1;
    if (!clip_rect(x, y, w, h, &x0, &y0, &x1, &y1)) return;

    for (int row = y0; row < y1; row++) {
        uint32_t* line = (uint32_t*)((uint8_t*)framebuffer + (uint64_t)(uint32_t)row * pitch);
        for (int col = x0; col < x1; col++) {
            const uint32_t destination = from_framebuffer_pixel(line[col]);
            line[col] = to_framebuffer_pixel(blend_pixel(destination, color, alpha));
        }
    }
}

void draw_rect_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    if (!framebuffer_available()) return;

    int x0, y0, x1, y1;
    if (!clip_rect(x, y, w, h, &x0, &y0, &x1, &y1)) return;

    uint32_t r_top = (top >> 16) & 0xFF, g_top = (top >> 8) & 0xFF, b_top = top & 0xFF;
    uint32_t r_bot = (bottom >> 16) & 0xFF, g_bot = (bottom >> 8) & 0xFF, b_bot = bottom & 0xFF;

    for (int row = y0; row < y1; row++) {
        int64_t rel = (int64_t)row - y;
        int t = h > 1 ? (int)(rel * 255 / (h - 1)) : 0;
        int inv = 255 - t;

        uint32_t r = (r_top * inv + r_bot * t) / 255;
        uint32_t g = (g_top * inv + g_bot * t) / 255;
        uint32_t b = (b_top * inv + b_bot * t) / 255;
        uint32_t color = 0xFF000000u | (r << 16) | (g << 8) | b;

        auto *line_ptr = reinterpret_cast<uint32_t *>(
            reinterpret_cast<uint8_t *>(framebuffer) +
            static_cast<uint64_t>(static_cast<uint32_t>(row)) * pitch) + x0;
        const uint32_t pixel = to_framebuffer_pixel(color);
        fill_framebuffer_pixels(line_ptr, pixel,
                                static_cast<uint32_t>(x1 - x0));
    }
}

void draw_rect_outline(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;

    draw_rect(x, y, w, 1, color);
    draw_rect(x, y, 1, h, color);

    int64_t right = (int64_t)x + w - 1;
    int64_t bottom = (int64_t)y + h - 1;
    if (right >= INT32_MIN && right <= INT32_MAX)
        draw_rect((int)right, y, 1, h, color);
    if (bottom >= INT32_MIN && bottom <= INT32_MAX)
        draw_rect(x, (int)bottom, w, 1, color);
}

static int64_t interpolate_clipped(int64_t base, int64_t delta,
                                   int64_t numerator, int64_t denominator) {
    if (!denominator) return base;
    const bool negative = (delta < 0) ^ (numerator < 0) ^ (denominator < 0);
    const uint64_t abs_delta = delta < 0
        ? static_cast<uint64_t>(-(delta + 1)) + 1
        : static_cast<uint64_t>(delta);
    const uint64_t abs_numerator = numerator < 0
        ? static_cast<uint64_t>(-(numerator + 1)) + 1
        : static_cast<uint64_t>(numerator);
    const uint64_t abs_denominator = denominator < 0
        ? static_cast<uint64_t>(-(denominator + 1)) + 1
        : static_cast<uint64_t>(denominator);
    const uint64_t magnitude = (abs_delta * abs_numerator) / abs_denominator;
    return base + (negative ? -static_cast<int64_t>(magnitude)
                            : static_cast<int64_t>(magnitude));
}

void draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    if (!framebuffer_available()) return;

    int64_t ax = x0, ay = y0, bx = x1, by = y1;
    auto out_code = [](int64_t x, int64_t y) -> uint8_t {
        uint8_t code = 0;
        if (x < 0) code |= 1;
        else if (x >= width) code |= 2;
        if (y < 0) code |= 4;
        else if (y >= height) code |= 8;
        return code;
    };

    for (;;) {
        uint8_t a_code = out_code(ax, ay);
        uint8_t b_code = out_code(bx, by);
        if ((a_code | b_code) == 0) break;
        if (a_code & b_code) return;

        uint8_t code = a_code ? a_code : b_code;
        int64_t x, y;
        if (code & 8) {
            if (by == ay) return;
            y = (int64_t)height - 1;
            x = interpolate_clipped(ax, bx - ax, y - ay, by - ay);
        } else if (code & 4) {
            if (by == ay) return;
            y = 0;
            x = interpolate_clipped(ax, bx - ax, y - ay, by - ay);
        } else if (code & 2) {
            if (bx == ax) return;
            x = (int64_t)width - 1;
            y = interpolate_clipped(ay, by - ay, x - ax, bx - ax);
        } else {
            if (bx == ax) return;
            x = 0;
            y = interpolate_clipped(ay, by - ay, x - ax, bx - ax);
        }

        if (code == a_code) {
            ax = x;
            ay = y;
        } else {
            bx = x;
            by = y;
        }
    }

    int64_t dx = bx >= ax ? bx - ax : ax - bx;
    int64_t dy = by >= ay ? -(by - ay) : -(ay - by);
    int64_t sx = ax < bx ? 1 : -1;
    int64_t sy = ay < by ? 1 : -1;
    int64_t err = dx + dy;
    while (true) {
        put_pixel((int)ax, (int)ay, color);
        if (ax == bx && ay == by) break;
        int64_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            ax += sx;
        }
        if (e2 <= dx) {
            err += dx;
            ay += sy;
        }
    }
}

static uint64_t integer_sqrt(uint64_t value) {
    uint64_t low = 0;
    uint64_t high = (uint64_t)INT32_MAX + 1u;
    while (low + 1 < high) {
        uint64_t mid = low + (high - low) / 2;
        if (mid <= value / mid) low = mid;
        else high = mid;
    }
    return low;
}

void draw_circule(int cx, int cy, int r, uint32_t color) {
    if (!framebuffer_available() || r <= 0) return;

    int64_t radius = r;
    uint64_t radius_squared = (uint64_t)(radius * radius);
    for (uint32_t row = 0; row < height; row++) {
        int64_t dy = (int64_t)row - cy;
        if (dy < -radius || dy > radius) continue;
        uint64_t remaining = radius_squared - (uint64_t)(dy * dy);
        int64_t dx = (int64_t)integer_sqrt(remaining);
        int64_t left = (int64_t)cx - dx;
        int64_t right = (int64_t)cx + dx;
        if (left >= 0 && left < width) put_pixel((int)left, (int)row, color);
        if (right >= 0 && right < width) put_pixel((int)right, (int)row, color);
    }

    for (uint32_t col = 0; col < width; col++) {
        int64_t dx = (int64_t)col - cx;
        if (dx < -radius || dx > radius) continue;
        uint64_t remaining = radius_squared - (uint64_t)(dx * dx);
        int64_t dy = (int64_t)integer_sqrt(remaining);
        int64_t top = (int64_t)cy - dy;
        int64_t bottom = (int64_t)cy + dy;
        if (top >= 0 && top < height) put_pixel((int)col, (int)top, color);
        if (bottom >= 0 && bottom < height) put_pixel((int)col, (int)bottom, color);
    }
}

void draw_filled_circle(int cx, int cy, int r, uint32_t color) {
    if (!framebuffer_available()) return;
    if (r <= 0) return;

    int64_t radius = r;
    int64_t r2 = radius * radius;
    int64_t y_start = -(int64_t)cy;
    int64_t y_end = (int64_t)height - 1 - cy;
    if (y_start < -radius) y_start = -radius;
    if (y_end > radius) y_end = radius;

    for (int64_t y = y_start; y <= y_end; y++) {
        int64_t y_pos = (int64_t)cy + y;
        int64_t x_start = -(int64_t)cx;
        int64_t x_end = (int64_t)width - 1 - cx;
        if (x_start < -radius) x_start = -radius;
        if (x_end > radius) x_end = radius;
        int64_t yy = y * y;
        for (int64_t x = x_start; x <= x_end; x++) {
            if (x * x + yy <= r2) {
                int64_t x_pos = (int64_t)cx + x;
                uint32_t* pixel = (uint32_t*)((uint8_t*)framebuffer + (uint64_t)y_pos * pitch) + x_pos;
                *pixel = to_framebuffer_pixel(color);
            }
        }
    }
}

void draw_char_font(int px, int py, char c, uint32_t fg, uint32_t bg,
                    int scale, uint32_t font) {
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    int font_width = 0, font_height = 0;
    if (!framebuffer_font_metrics(font, &font_width, &font_height)) return;
    uint16_t idx = (uint16_t)c;
    if (idx >= 128) idx = '?';
    bool transparent_bg = ((bg >> 24) == 0);
    for (int row = 0; row < font_height; row++) {
        uint16_t row_bits = 0;
        uint8_t system8_bits = 0;
        if (font == WELOVEC_FB_FONT_SYSTEM16) {
            // system16 preserves the original renderer: an 8x8 glyph doubled
            // in both dimensions to fill a 16x16 cell.
            const uint8_t bits = font16x16_basic[idx][row / 2];
            for (int col = 0; col < font_width; ++col)
                if (bits & (1u << (col / 2)))
                    row_bits |= static_cast<uint16_t>(1u << (15 - col));
        } else if (font == WELOVEC_FB_FONT_SYSTEM8) {
            system8_bits = font8x8_basic[idx][row];
        } else {
            row_bits = font_ubuntu_bitmap_16x16[idx][row];
        }
        for (int col = 0; col < font_width; col++) {
            const bool glyph_pixel = font == WELOVEC_FB_FONT_SYSTEM8
                ? (system8_bits & (1u << col)) != 0
                : (row_bits & (1u << (font_width - 1 - col))) != 0;
            if (!glyph_pixel && transparent_bg) continue;
            uint32_t color = glyph_pixel ? fg : bg;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    int64_t x = (int64_t)px + col * scale + sx;
                    int64_t y = (int64_t)py + row * scale + sy;
                    if (x >= 0 && x < width && y >= 0 && y < height)
                        put_pixel((int)x, (int)y, color);
                }
            }
        }
    }
}

void draw_char(int px, int py, char c, uint32_t fg, uint32_t bg, int scale) {
    draw_char_font(px, py, c, fg, bg, scale, WELOVEC_FB_FONT_SYSTEM16);
}

void print(int x, int y, const char* str, uint32_t color) {
    if (!str) return;
    fb_scrollback_return_live();
    int px = x;
    while(*str) {
        char c = *str++;
        if (c == '\n') {
            y += FB_FONT_HEIGHT * fb_char_scale;
            px = x;
            continue;
        }
        draw_char(px, y, c, color, fb_bg_color, fb_char_scale);
        px += FB_FONT_WIDTH * fb_char_scale;
    }
}

static void fb_scroll_up_pixels(int char_h) {
    if (!framebuffer_available() || char_h <= 0) return;
    fb_scrollback_return_live();

    if ((uint32_t)char_h >= height) {
        fb_clear_screen();
        return;
    }
    capture_scrollback_rows(static_cast<uint32_t>(char_h));
    const uint64_t row_bytes = static_cast<uint64_t>(width) * sizeof(uint32_t);
    const uint32_t rows_to_move = height - static_cast<uint32_t>(char_h);
    auto *base = reinterpret_cast<uint8_t *>(framebuffer);
    if (pitch == row_bytes) {
        copy_forward_bytes(base,
            base + static_cast<uint64_t>(char_h) * pitch,
            row_bytes * rows_to_move);
    } else {
        for (uint32_t row = 0; row < rows_to_move; ++row) {
            uint8_t *destination = base + static_cast<uint64_t>(row) * pitch;
            const uint8_t *source = base +
                (static_cast<uint64_t>(row) + static_cast<uint32_t>(char_h)) * pitch;
            copy_forward_bytes(destination, source, row_bytes);
        }
    }

    const uint32_t fill = to_framebuffer_pixel(fb_bg_color);
    for (uint32_t row = rows_to_move; row < height; ++row) {
        auto *line = reinterpret_cast<uint32_t *>(base +
            static_cast<uint64_t>(row) * pitch);
        fill_framebuffer_pixels(line, fill, width);
    }
}

static void fb_scroll_up() {
    fb_scroll_up_pixels(FB_FONT_HEIGHT * fb_char_scale);
}

void fb_clear_screen_with_color(uint32_t color) {
    if (!framebuffer_available()) return;
    fb_scrollback_return_live();

    const uint32_t pixel = to_framebuffer_pixel(color);
    for (uint32_t row = 0; row < height; row++) {
        uint32_t* line = (uint32_t*)((uint8_t*)framebuffer + (uint64_t)row * pitch);
        fill_framebuffer_pixels(line, pixel, width);
    }
    fb_cursor_x = FB_MARGIN;
    fb_cursor_y = FB_MARGIN;
    if (!gVirtualFramebufferActive) {
        gScrollbackRowCount = 0;
        gScrollbackWriteRow = 0;
        gScrollbackViewOffset = 0;
    }
}

void fb_clear_screen() {
    fb_clear_screen_with_color(fb_bg_color);
}

void fb_reset_print_cursor(void) {
    fb_cursor_x = FB_MARGIN;
    fb_cursor_y = FB_MARGIN;
}

static void putchar_fb_with_color(char c, uint32_t color) {
    fb_scrollback_return_live();
    int char_w = FB_FONT_WIDTH * fb_char_scale;
    int char_h = FB_FONT_HEIGHT * fb_char_scale;
    if (c == '\n') {
        fb_cursor_y += char_h;
        fb_cursor_x = FB_MARGIN;
    }
    else if (c == '\r') {
        fb_cursor_x = FB_MARGIN;
    }
    else if (c == '\b') {
        fb_cursor_x -= char_w;
        if (fb_cursor_x < FB_MARGIN) fb_cursor_x = FB_MARGIN;
        draw_rect(fb_cursor_x, fb_cursor_y, char_w, char_h, fb_bg_color);

        return;
    }
    else if (c == '\t') {
        int tab_w = char_w * 4;
        fb_cursor_x = ((fb_cursor_x - FB_MARGIN) / tab_w + 1) * tab_w + FB_MARGIN;
    }
    else {
        draw_char(fb_cursor_x, fb_cursor_y, c, color, fb_bg_color, fb_char_scale);
        fb_cursor_x += char_w;
    }

    if (fb_cursor_x + char_w > (int)width - FB_MARGIN) {
        fb_cursor_x = FB_MARGIN;
        fb_cursor_y += char_h;
    }

    if (fb_cursor_y + char_h > (int)height - FB_MARGIN) {
        fb_scroll_up();
        fb_cursor_y -= char_h;
        if (fb_cursor_y < FB_MARGIN) fb_cursor_y = FB_MARGIN;
    }
        
}

void putchar_fb(char c) {
    putchar_fb_font(c, fb_char_color, WELOVEC_FB_FONT_SYSTEM16);
}

void putchar_fb_font(char c, uint32_t color, uint32_t font) {
    fb_scrollback_return_live();
    int font_width = 0, font_height = 0;
    if (!framebuffer_font_metrics(font, &font_width, &font_height)) return;

    const int char_w = font_width * fb_char_scale;
    const int char_h = font_height * fb_char_scale;
    if (c == '\n') {
        fb_cursor_y += char_h;
        fb_cursor_x = FB_MARGIN;
    } else if (c == '\r') {
        fb_cursor_x = FB_MARGIN;
    } else if (c == '\b') {
        fb_cursor_x -= char_w;
        if (fb_cursor_x < FB_MARGIN) fb_cursor_x = FB_MARGIN;
        draw_rect(fb_cursor_x, fb_cursor_y, char_w, char_h, fb_bg_color);
        return;
    } else if (c == '\t') {
        const int tab_w = char_w * 4;
        fb_cursor_x = ((fb_cursor_x - FB_MARGIN) / tab_w + 1) * tab_w + FB_MARGIN;
    } else {
        draw_char_font(fb_cursor_x, fb_cursor_y, c, color, fb_bg_color,
                       fb_char_scale, font);
        fb_cursor_x += char_w;
    }

    if (fb_cursor_x + char_w > (int)width - FB_MARGIN) {
        fb_cursor_x = FB_MARGIN;
        fb_cursor_y += char_h;
    }
    if (fb_cursor_y + char_h > (int)height - FB_MARGIN) {
        fb_scroll_up_pixels(char_h);
        fb_cursor_y -= char_h;
        if (fb_cursor_y < FB_MARGIN) fb_cursor_y = FB_MARGIN;
    }
}

void print_fb_font(const char *text, uint32_t color, uint32_t font) {
    if (!text || !framebuffer_font_valid(font)) return;
    for (uint64_t index = 0; text[index] != '\0'; ++index)
        putchar_fb_font(text[index], color, font);
}

static int int_to_str(char* buf, int bufsize, uint32_t val, int base, bool uppercase) {
    const char* digits_lo = "0123456789abcdef";
    const char* digital_hi = "0123456789ABCDEF";
    const char* digitds = uppercase ? digital_hi : digits_lo;

    if (!buf || bufsize <= 0) return 0;
    if (base < 2 || base > 16 || val == 0) {
        if (bufsize > 1) { buf[0] = '0'; buf[1] = '\0'; return 1; }
        buf[0] = '\0';
        return 0;
    }

    char tmp[32];
    int len = 0;
    while (val > 0 && len < 31) {
        tmp[len++] = digitds[val % (uint32_t)base];
        val /= (uint32_t)base;
    }

    int out = 0;
    for (int i = len - 1; i >= 0 && out + 1 < bufsize; i--) {
        buf[out++] = tmp[i];
    }
    buf[out] = '\0';
    return out;
}

void printf_fb(const char* fmt) {
    if (!fmt) return;
    for (int i = 0; fmt[i] != '\0'; i++) {
        putchar_fb(fmt[i]);
    }
}

void printf_fb_color(const char* str, uint32_t color) {
    if (!str) return;
    for (int i = 0; str[i] != '\0'; i++) {
        putchar_fb_with_color(str[i], color);
    }
}

void printf_fb_s(const char* fmt, const char* arg) {
    if (!fmt) return;
    if (!arg) arg = "(null)";
    for (int i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] == '%' && fmt[i + 1] == 's') {
            for (int j = 0; arg[j] != '\0'; j++) {
                putchar_fb(arg[j]);
            }
            i++;
        }
        else {
            putchar_fb(fmt[i]);
        }
    }
}

void printf_fb_d(const char* fmt, int32_t arg) {
    if (!fmt) return;
    for (int i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] == '%' && (fmt[i + 1] == 'd' || fmt[i + 1] == 'i')) {
            char buf[16];
            uint32_t uval = (uint32_t)arg;
          if (arg < 0) {
            putchar_fb('-');
            uval = 0u - (uint32_t)arg;
          }
          int_to_str(buf, 16, uval, 10, false);
          for (int j = 0; buf[j]; j++) {
            putchar_fb(buf[j]);
          }
          i++;
        }
        else if (fmt[i] == '%' && fmt[i + 1] == 'x') {
            char buf[16];
            int_to_str(buf, 16, (uint32_t)arg, 16, false);
            for (int j = 0; buf[j]; j++) putchar_fb(buf[j]);
            i++;
        }
        else if (fmt[i] == '%' && fmt[i + 1] == 'X') {
            char buf[16];
            int_to_str(buf, 16, (uint32_t)arg, 16, true);
            for ( int j = 0; buf[j]; j++) putchar_fb(buf[j]);
            i++;
        }
        else {
            putchar_fb(fmt[i]);
        }

    }    
}

int fb_get_cursor_x() { return fb_cursor_x; }
int fb_get_cursor_y() { return fb_cursor_y; }
void fb_set_cursor(int x, int y) {
    fb_cursor_x = x;
    fb_cursor_y = y;
}
