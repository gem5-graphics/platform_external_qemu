/* Copyright (C) 2015 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

#include <QtCore>
#include <QColor>
#include <QImage>
#include <QObject>
#include <QPainter>
#include <QRect>
#include <QWidget>

#include "android/skin/argb.h"
#include "android/skin/rect.h"
#include "android/skin/surface.h"
#include "android/skin/winsys.h"
#include "android/skin/qt/emulator-qt-window.h"
#include "android/utils/setenv.h"

#include <memory>

#define  DEBUG  1

#if DEBUG
#include "android/utils/debug.h"

#define  D(...)   VERBOSE_PRINT(surface,__VA_ARGS__)
#else
#define  D(...)   ((void)0)
#endif

static int next_id = 0;

static void skin_surface_free(SkinSurface *s)
{
    D("skin_surface_free %d", s->id);
    s->window->releaseBitmap(s, nullptr);
}

extern int skin_surface_height(SkinSurface *s)
{
    D("skin_surface_height %d", s->id);
    return s->h;
}

extern void skin_surface_unrefp(SkinSurface* *psurface)
{
    SkinSurface *surf = *psurface;
    if (surf) {
        D("skin_surface_unref %d", surf->id);
        skin_surface_free(surf);
        *psurface = NULL;
    }
}

extern int skin_surface_width(SkinSurface *s)
{
    D("skin_surface_width %d", s->id);
    return s->w;
}

template <class FillBitmapFunc>
static SkinSurface* createSkinSurface(FillBitmapFunc&& fillBitmap) {
    auto s = std::unique_ptr<SkinSurface>(new SkinSurface());
    if (!s) {
        return nullptr;
    }
    s->window = EmulatorQtWindow::getInstancePtr();
    if (!s->window) {
        return nullptr;
    }
    fillBitmap(s.get());
    if (!s->bitmap) {
        return nullptr;
    }
    s->id = next_id++;
    return s.release();
}

extern SkinSurface *skin_surface_create(int w, int h, int original_w, int original_h)
{
    return createSkinSurface([w, h, original_w, original_h](SkinSurface* s) {
        s->bitmap = new SkinSurfaceBitmap(original_w, original_h);
        s->w = w;
        s->h = h;
    });
}

extern SkinSurface* skin_surface_create_from_data(const void* data, int size) {
    return createSkinSurface([data, size](SkinSurface* s) {
        s->bitmap = new SkinSurfaceBitmap((const unsigned char*)data, size);
        s->w = s->bitmap->size().width();
        s->h = s->bitmap->size().height();
    });
}

extern SkinSurface* skin_surface_create_from_file(const char* path) {
    return createSkinSurface([path](SkinSurface* s) {
        s->bitmap = new SkinSurfaceBitmap(path);
        s->w = s->bitmap->size().width();
        s->h = s->bitmap->size().height();
    });
}

extern SkinSurface* skin_surface_create_derived(SkinSurface* source,
                                                SkinRotation rotation,
                                                int blend) {
    return createSkinSurface([source, rotation, blend](SkinSurface* s) {
        s->w = source->w;
        s->h = source->h;
        if (rotation == SKIN_ROTATION_90 || rotation == SKIN_ROTATION_270) {
            std::swap(s->w, s->h);
        }
        s->bitmap = new SkinSurfaceBitmap(*source->bitmap, rotation, blend);
    });
}


extern SkinSurface* skin_surface_resize(SkinSurface *surface, int w, int h,
                                        int original_w, int original_h)
{
    if ( surface == NULL ) {
        return skin_surface_create(w, h, original_w, original_h);
    } else if (surface->bitmap->size() == QSize(original_w, original_h)) {
        surface->w = w;
        surface->h = h;
        return surface;
    } else {
        skin_surface_unrefp(&surface);
        return skin_surface_create(w, h, original_w, original_h);
    }
}

extern void skin_surface_create_window(SkinSurface* surface,
                                       int x,
                                       int y,
                                       int w,
                                       int h) {
    D("skin_surface_create_window  %d, %d, %d, %d", x, y, w, h);
    EmulatorQtWindow *window = EmulatorQtWindow::getInstance();
    if (window == NULL) return;
    QRect rect(x, y, w, h);
    window->showWindow(surface, rect, nullptr);
    D("ID of backing bitmap surface is %d", surface->id);
}

extern void skin_surface_update(SkinSurface *surface, SkinRect *rect)
{
#if 0
    D("skin_surface_update %d: %d,%d,%d,%d", surface->id, rect->pos.x, rect->pos.y, rect->size.w, rect->size.h);
#endif
    QRect qrect(rect->pos.x, rect->pos.y, rect->size.w, rect->size.h);
    surface->window->requestUpdate(qrect, nullptr);
}

extern void skin_surface_blit(SkinSurface *dst, SkinPos *pos, SkinSurface *src, SkinRect *rect, SkinBlitOp op)
{
#if 0
    D("skin_surface_blit from %d (%d, %d) to %d: %d,%d,%d,%d", src->id, rect->pos.x, rect->pos.y, dst->id, rect->pos.x, rect->pos.y, rect->size.w, rect->size.h);
#endif
    QRect qrect(rect->pos.x, rect->pos.y, rect->size.w, rect->size.h);
    QPoint qpos(pos->x, pos->y);
    QPainter::CompositionMode qop;
    switch(op) {
        default:
        case SKIN_BLIT_COPY:
            qop = QPainter::CompositionMode_Source;
            break;
        case SKIN_BLIT_SRCOVER:
            qop = QPainter::CompositionMode_SourceOver;
            break;
    }
    dst->window->blit(src->bitmap, qrect, dst->bitmap, qpos, qop, nullptr);
}

extern void skin_surface_fill(SkinSurface *dst, SkinRect *rect, uint32_t argb_premul)
{
    D("skin_surface_fill %d: %d, %d, %d, %d: %x", dst->id, rect->pos.x, rect->pos.y, rect->size.w, rect->size.h, argb_premul);
    QRect qrect(rect->pos.x, rect->pos.y, rect->size.w, rect->size.h);
    QColor color(argb_premul);
    dst->window->fill(dst, qrect, color, nullptr);
}

extern void skin_surface_upload(SkinSurface *surface, const SkinRect *rect, const void *pixels, int pitch)
{
    D("skin_surface_upload %d: %d,%d,%d,%d", surface->id, rect->pos.x, rect->pos.y, rect->size.w, rect->size.h);
    assert(QSize(surface->w, surface->h) == surface->bitmap->size());
    if (rect->pos.x == 0 && rect->pos.y == 0 &&
        QSize(rect->size.w, rect->size.h) == surface->bitmap->size() &&
        pitch == rect->size.w * 4) {
        memcpy(surface->bitmap->get().bits(), pixels, surface->h * surface->w * 4);
    } else {
        const uint32_t *src = (const uint32_t*)pixels;
        uint32_t *dst = ((uint32_t*)surface->bitmap->get().bits()) + surface->w * rect->pos.y;
        for (int y = rect->pos.y; y < rect->pos.y + rect->size.h; y++) {
            for (int x = rect->pos.x; x < rect->pos.x + rect->size.w; x++) {
                *(dst + x) = *(src + x - rect->pos.x);
            }
            src += pitch / sizeof(uint32_t);
            dst += surface->w;
        }
    }
}

extern void skin_surface_get_scaled_rect(SkinSurface *surface, const SkinRect *from, SkinRect *to)
{
    int fromx = from->pos.x;
    int fromy = from->pos.y;
    int fromw = from->size.w;
    int fromh = from->size.h;
    int w = surface->w;
    int h = surface->h;
    int original_w = surface->bitmap->size().width();
    int original_h = surface->bitmap->size().height();
    to->pos.x = from->pos.x * w / original_w;
    to->pos.y = from->pos.y * h / original_h;
    to->size.w = from->size.w * w / original_w;
    to->size.h = from->size.h * h / original_h;
    D("skin_surface_get_scaled_rect %d: %d, %d, %d, %d => %d, %d, %d, %d", surface->id, fromx, fromy, fromw, fromh, to->pos.x, to->pos.y, to->size.w, to->size.h);
}

extern void skin_surface_reverse_map(SkinSurface *surface, int *x, int *y)
{
    int new_x = *x * surface->bitmap->size().width() / surface->w;
    int new_y = *y * surface->bitmap->size().height() / surface->h;
#if 0
    D("skin_surface_reverse_map %d: %d,%d to %d,%d", surface->id, *x, *y, new_x, new_y);
#endif
    *x = new_x;
    *y = new_y;
}
