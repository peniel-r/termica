/*
 * termica - image placements from libghostty's render state. See image.h.
 */
#include "image.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* How many images keep a texture at once. A slot is recycled least-recently
 * used, so a program that scrolls through screenshots cannot grow this. */
#define IMAGE_CACHE_LIMIT 32

/* Textures cost memory even after the terminal drops an image, so the cache
 * also has a byte budget. Raw RGBA is four bytes per pixel. */
#define IMAGE_CACHE_BYTES (64u * 1024u * 1024u)

typedef struct ImageEntry {
    uint32_t     id;
    uint64_t     generation;
    uint64_t     last_used;
    size_t       bytes;
    SDL_Texture *texture;
} ImageEntry;

struct ImageCache {
    SDL_Renderer *renderer;
    ImageEntry    entries[IMAGE_CACHE_LIMIT];
    size_t        bytes;
    uint64_t      tick;
};

/* One placement, after the fields we need have been read. */
typedef struct Placement {
    uint32_t  image_id;
    uint64_t  generation;
    int32_t   z;
    int32_t   column;
    int32_t   row;
    uint32_t  offset_x;
    uint32_t  offset_y;
    uint32_t  pixel_width;
    uint32_t  pixel_height;
    uint32_t  source_x;
    uint32_t  source_y;
    uint32_t  source_width;
    uint32_t  source_height;
    size_t    order;
} Placement;

static void cache_release(ImageCache *cache)
{
    int i;

    for (i = 0; i < IMAGE_CACHE_LIMIT; i++) {
        if (cache->entries[i].texture)
            SDL_DestroyTexture(cache->entries[i].texture);
        memset(&cache->entries[i], 0, sizeof cache->entries[i]);
    }
    cache->bytes = 0;
    cache->tick = 0;
}

void images_free(ImageCache **cache)
{
    if (!cache || !*cache)
        return;
    cache_release(*cache);
    SDL_free(*cache);
    *cache = NULL;
}

static ImageCache *cache_get(ImageCache **cache, SDL_Renderer *renderer)
{
    ImageCache *result = *cache;

    if (!result) {
        result = SDL_calloc(1, sizeof *result);
        if (!result)
            return NULL;
        *cache = result;
    }
    if (result->renderer != renderer) {
        cache_release(result);
        result->renderer = renderer;
    }
    return result;
}

/* The image's pixels, converted to the RGBA32 layout SDL wants. The caller
 * owns the returned buffer. */
static uint8_t *image_pixels(Term *term, uint32_t id, uint32_t *out_width,
                             uint32_t *out_height)
{
    GhosttyKittyGraphics graphics = NULL;
    GhosttyKittyGraphicsImage image;
    GhosttyKittyImageFormat format = GHOSTTY_KITTY_IMAGE_FORMAT_RGBA;
    GhosttyString pixels = { 0 };
    uint32_t width = 0;
    uint32_t height = 0;
    size_t count;
    size_t index;
    uint8_t *rgba;

    if (!term || !term->handle)
        return NULL;
    if (ghostty_terminal_get(term->handle, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,
                             &graphics) != GHOSTTY_SUCCESS ||
        !graphics)
        return NULL;
    image = ghostty_kitty_graphics_image(graphics, id);
    if (!image)
        return NULL;
    if (ghostty_kitty_graphics_image_get(image,
            GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_WIDTH,
            &width) != GHOSTTY_SUCCESS ||
        ghostty_kitty_graphics_image_get(image,
            GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_HEIGHT,
            &height) != GHOSTTY_SUCCESS ||
        ghostty_kitty_graphics_image_get(image,
            GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_FORMAT,
            &format) != GHOSTTY_SUCCESS ||
        ghostty_kitty_graphics_image_get(image,
            GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_PIXELS,
            &pixels) != GHOSTTY_SUCCESS)
        return NULL;
    if (width == 0 || height == 0 || !pixels.ptr ||
        (size_t)width > SIZE_MAX / (size_t)height)
        return NULL;
    count = (size_t)width * (size_t)height;
    if (count > SIZE_MAX / 4)
        return NULL;

    rgba = SDL_malloc(count * 4);
    if (!rgba)
        return NULL;

    switch (format) {
    case GHOSTTY_KITTY_IMAGE_FORMAT_RGB:
        if (pixels.len < count * 3) {
            SDL_free(rgba);
            return NULL;
        }
        for (index = 0; index < count; index++) {
            rgba[index * 4 + 0] = pixels.ptr[index * 3 + 0];
            rgba[index * 4 + 1] = pixels.ptr[index * 3 + 1];
            rgba[index * 4 + 2] = pixels.ptr[index * 3 + 2];
            rgba[index * 4 + 3] = 255;
        }
        break;
    case GHOSTTY_KITTY_IMAGE_FORMAT_RGBA:
        if (pixels.len < count * 4) {
            SDL_free(rgba);
            return NULL;
        }
        memcpy(rgba, pixels.ptr, count * 4);
        break;
    case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY:
        if (pixels.len < count) {
            SDL_free(rgba);
            return NULL;
        }
        for (index = 0; index < count; index++) {
            uint8_t value = pixels.ptr[index];

            rgba[index * 4 + 0] = value;
            rgba[index * 4 + 1] = value;
            rgba[index * 4 + 2] = value;
            rgba[index * 4 + 3] = 255;
        }
        break;
    case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY_ALPHA:
        if (pixels.len < count * 2) {
            SDL_free(rgba);
            return NULL;
        }
        for (index = 0; index < count; index++) {
            uint8_t value = pixels.ptr[index * 2 + 0];

            rgba[index * 4 + 0] = value;
            rgba[index * 4 + 1] = value;
            rgba[index * 4 + 2] = value;
            rgba[index * 4 + 3] = pixels.ptr[index * 2 + 1];
        }
        break;
    default:
        SDL_free(rgba);
        return NULL;
    }

    *out_width = width;
    *out_height = height;
    return rgba;
}

static SDL_Texture *upload_texture(SDL_Renderer *renderer, Term *term,
                                   uint32_t id, uint32_t *out_width,
                                   uint32_t *out_height)
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t *pixels = image_pixels(term, id, &width, &height);
    SDL_Texture *texture;

    if (!pixels)
        return NULL;
    if (width > (uint32_t)INT_MAX || height > (uint32_t)INT_MAX ||
        width > (uint32_t)INT_MAX / 4) {
        SDL_free(pixels);
        return NULL;
    }
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                SDL_TEXTUREACCESS_STATIC, (int)width,
                                (int)height);
    if (texture &&
        (!SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND) ||
         !SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR) ||
         !SDL_UpdateTexture(texture, NULL, pixels, (int)width * 4))) {
        SDL_DestroyTexture(texture);
        texture = NULL;
    }
    SDL_free(pixels);
    *out_width = width;
    *out_height = height;
    return texture;
}

/* Find the texture for (id, generation), uploading it when the cache is cold
 * or the pixels changed. */
static SDL_Texture *image_texture(ImageCache *cache, SDL_Renderer *renderer,
                                  Term *term, uint32_t id,
                                  uint64_t generation)
{
    ImageEntry *entry = NULL;
    SDL_Texture *texture;
    uint32_t width = 0;
    uint32_t height = 0;
    size_t bytes;
    int i;

    cache->tick++;
    for (i = 0; i < IMAGE_CACHE_LIMIT; i++) {
        if (cache->entries[i].texture && cache->entries[i].id == id) {
            entry = &cache->entries[i];
            break;
        }
    }
    if (entry && entry->generation == generation) {
        entry->last_used = cache->tick;
        return entry->texture;
    }

    texture = upload_texture(renderer, term, id, &width, &height);
    if (!texture)
        return NULL;
    if ((size_t)height > SIZE_MAX / 4 / (size_t)width) {
        SDL_DestroyTexture(texture);
        return NULL;
    }
    bytes = (size_t)width * (size_t)height * 4;

    /* Drop the old version of this image first. */
    if (entry) {
        SDL_DestroyTexture(entry->texture);
        entry->texture = NULL;
        entry->id = 0;
        cache->bytes -= entry->bytes;
    }

    /* Make room by evicting the least-recently used textures. A single image
     * larger than the budget is still drawn; it just leaves no room for
     * others until it stops being used. */
    while (cache->bytes + bytes > IMAGE_CACHE_BYTES) {
        ImageEntry *victim = NULL;

        for (i = 0; i < IMAGE_CACHE_LIMIT; i++) {
            if (cache->entries[i].texture &&
                (!victim || cache->entries[i].last_used < victim->last_used))
                victim = &cache->entries[i];
        }
        if (!victim)
            break;
        SDL_DestroyTexture(victim->texture);
        victim->texture = NULL;
        victim->id = 0;
        cache->bytes -= victim->bytes;
    }

    if (!entry) {
        for (i = 0; i < IMAGE_CACHE_LIMIT; i++) {
            if (!cache->entries[i].texture) {
                entry = &cache->entries[i];
                break;
            }
        }
    }
    if (!entry) {
        entry = &cache->entries[0];
        for (i = 1; i < IMAGE_CACHE_LIMIT; i++) {
            if (cache->entries[i].last_used < entry->last_used)
                entry = &cache->entries[i];
        }
        SDL_DestroyTexture(entry->texture);
        entry->texture = NULL;
        cache->bytes -= entry->bytes;
    }
    entry->id = id;
    entry->generation = generation;
    entry->last_used = cache->tick;
    entry->bytes = bytes;
    entry->texture = texture;
    cache->bytes += bytes;
    return texture;
}

static bool read_placement(GhosttyRenderStateImageIterator iterator,
                           GhosttyImageLayer layer, Placement *placement)
{
    GhosttyImageProtocol protocol;
    GhosttyImageLayer placement_layer;

    if (ghostty_render_state_image_get(iterator,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_PROTOCOL,
            &protocol) != GHOSTTY_SUCCESS ||
        protocol != GHOSTTY_IMAGE_PROTOCOL_KITTY)
        return false;
    if (ghostty_render_state_image_get(iterator,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_LAYER,
            &placement_layer) != GHOSTTY_SUCCESS ||
        placement_layer != layer)
        return false;
    if (ghostty_render_state_image_get(iterator,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_ID,
            &placement->image_id) != GHOSTTY_SUCCESS ||
        ghostty_render_state_image_get(iterator,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_GENERATION,
            &placement->generation) != GHOSTTY_SUCCESS ||
        ghostty_render_state_image_get(iterator,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_Z,
            &placement->z) != GHOSTTY_SUCCESS ||
        ghostty_render_state_image_get(iterator,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_VIEWPORT_COL,
            &placement->column) != GHOSTTY_SUCCESS ||
        ghostty_render_state_image_get(iterator,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_VIEWPORT_ROW,
            &placement->row) != GHOSTTY_SUCCESS ||
        ghostty_render_state_image_get(iterator,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_CELL_OFFSET_X,
            &placement->offset_x) != GHOSTTY_SUCCESS ||
        ghostty_render_state_image_get(iterator,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_CELL_OFFSET_Y,
            &placement->offset_y) != GHOSTTY_SUCCESS ||
        ghostty_render_state_image_get(iterator,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_PIXEL_WIDTH,
            &placement->pixel_width) != GHOSTTY_SUCCESS ||
        ghostty_render_state_image_get(iterator,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_PIXEL_HEIGHT,
            &placement->pixel_height) != GHOSTTY_SUCCESS ||
        ghostty_render_state_image_get(iterator,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_X,
            &placement->source_x) != GHOSTTY_SUCCESS ||
        ghostty_render_state_image_get(iterator,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_Y,
            &placement->source_y) != GHOSTTY_SUCCESS ||
        ghostty_render_state_image_get(iterator,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_WIDTH,
            &placement->source_width) != GHOSTTY_SUCCESS ||
        ghostty_render_state_image_get(iterator,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_HEIGHT,
            &placement->source_height) != GHOSTTY_SUCCESS)
        return false;
    return placement->pixel_width > 0 && placement->pixel_height > 0 &&
           placement->source_width > 0 && placement->source_height > 0;
}

/* Draw one placement with its destination clipped to the grid. The source
 * rectangle is adjusted by the same fraction, so a partially scrolled image
 * shows the right part of itself. A placement that cannot be drawn is
 * skipped: images are untrusted content and must not fail the frame. */
static void draw_placement(ImageCache *cache, SDL_Renderer *renderer,
                           Term *term, const Placement *placement, int left,
                           int top, int width, int height)
{
    SDL_Texture *texture = image_texture(cache, renderer, term,
                                         placement->image_id,
                                         placement->generation);
    double dest_x;
    double dest_y;
    double dest_width;
    double dest_height;
    double clip_left;
    double clip_top;
    double clip_right;
    double clip_bottom;
    SDL_FRect source;
    SDL_FRect destination;

    if (!texture)
        return;

    dest_x = left + (double)placement->column * term->cell_width +
             (double)placement->offset_x;
    dest_y = top + (double)placement->row * term->cell_height +
             (double)placement->offset_y;
    dest_width = placement->pixel_width;
    dest_height = placement->pixel_height;

    clip_left = dest_x > (double)left ? dest_x : (double)left;
    clip_top = dest_y > (double)top ? dest_y : (double)top;
    clip_right = dest_x + dest_width < (double)(left + width)
                     ? dest_x + dest_width
                     : (double)(left + width);
    clip_bottom = dest_y + dest_height < (double)(top + height)
                      ? dest_y + dest_height
                      : (double)(top + height);
    if (clip_right <= clip_left || clip_bottom <= clip_top)
        return;

    source.x = (float)((double)placement->source_x +
                       (clip_left - dest_x) * placement->source_width /
                           dest_width);
    source.y = (float)((double)placement->source_y +
                       (clip_top - dest_y) * placement->source_height /
                           dest_height);
    source.w = (float)((clip_right - clip_left) * placement->source_width /
                       dest_width);
    source.h = (float)((clip_bottom - clip_top) * placement->source_height /
                       dest_height);
    destination.x = (float)clip_left;
    destination.y = (float)clip_top;
    destination.w = (float)(clip_right - clip_left);
    destination.h = (float)(clip_bottom - clip_top);

    (void)SDL_RenderTexture(renderer, texture, &source, &destination);
}

static int compare_placements(const void *a, const void *b)
{
    const Placement *left = (const Placement *)a;
    const Placement *right = (const Placement *)b;

    if (left->z != right->z)
        return left->z < right->z ? -1 : 1;
    /* Kitty makes the lower image id lower within equal z-indexes. */
    if (left->image_id != right->image_id)
        return left->image_id < right->image_id ? -1 : 1;
    if (left->order != right->order)
        return left->order < right->order ? -1 : 1;
    return 0;
}

void images_draw(ImageCache **cache_pointer, SDL_Renderer *renderer, Term *term,
                 GhosttyImageLayer layer, int left, int top, int width,
                 int height)
{
    ImageCache *cache;
    Placement *placements = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t index;

    if (!cache_pointer || !renderer || !term || !term->render_state ||
        width <= 0 || height <= 0)
        return;
    cache = cache_get(cache_pointer, renderer);
    if (!cache)
        return;
    if (ghostty_render_state_get(term->render_state,
            GHOSTTY_RENDER_STATE_DATA_IMAGE_ITERATOR,
            &term->image_iterator) != GHOSTTY_SUCCESS)
        return;

    while (ghostty_render_state_image_iterator_next(term->image_iterator)) {
        Placement placement;

        memset(&placement, 0, sizeof placement);
        if (!read_placement(term->image_iterator, layer, &placement))
            continue;
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            Placement *grown;

            if (next > SIZE_MAX / sizeof *grown) {
                SDL_free(placements);
                return;
            }
            grown = SDL_realloc(placements, next * sizeof *grown);
            if (!grown) {
                SDL_free(placements);
                return;
            }
            placements = grown;
            capacity = next;
        }
        placement.order = count;
        placements[count++] = placement;
    }

    if (count > 1)
        qsort(placements, count, sizeof *placements, compare_placements);
    for (index = 0; index < count; index++)
        draw_placement(cache, renderer, term, &placements[index], left, top,
                       width, height);
    SDL_free(placements);
}
