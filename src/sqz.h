﻿/**
 * \file            sqz.h
 * \brief           SQZ image compression library
 */

/*
    SQZ - Low complexity, scalable lossless and lossy image compression library

                    Copyright (c) 2024, Márcio Pais

                    SPDX-License-Identifier: MIT

SQZ is a simple image codec designed to be scalable at a byte-level granularity,
providing lossless to extremelly low-rate lossy image compression without the need
for multiple images, simply by truncating an image at the required allocation budget.

It is responsive by design to the fullest extreme - encode once, serve many - where
every single additional byte allows for progressivelly better image quality, without
the need to reencode on-the-fly or store multiple versions of the same image.

(1) Technical details

SQZ uses a run-length wavelet bitplane encoding scheme with no entropy coding.
The chosen wavelet is the integer reversible 5/3 wavelet used in a myriad of other
codecs, and each subband bitplane is coded using a simple 2 stage DWT coefficient
significance and refinement method.

The subbands are scanned in one of 4 possible scan orders (raster, snake, Morton or
Hilbert), and the sorting pass encodes the linear distances between new significant
coefficients at each bitplane using the wavelet difference reduction method (WDR).
Internally, the image pixel data is stored in one of 4 color modes - 8bpp grayscale,
YCoCg-R, Oklab or logl1 - where the last 2 do not allow for lossless sRGB conversion.
For lossy compression of photographic images, the Oklab perceptual colorspace can
provide a significant gain in terms of perceived subjective image quality, at the
cost of increased computational load.

The DWT subband tree is encoded according to a schedule optimized for higher rate
compression, to try to squeeze the best possible subjective quality as early as
possible. An optional flag can be set to further delay encoding of the bitplanes
from the chroma planes, which acts as subsampling when doing lossy compression.
For lossless compression, none of this affects the final compressed size, as only
the priority of the bits being sent is changed.

A compressed SQZ bitstream starts with a compact 6 byte header:
     - Magic        [1 byte] ("0xA5")
     - Width        [2 bytes] (Big-Endian)
     - Height       [2 bytes] (Big-Endian)
     - Color mode   [1 bit]
     - DWT levels   [3 bits]
     - Scan order   [2 bits]
     - Subsampling  [1 bit]

No other information is stored, as it strives to provide the best possible LQIP
at every byte allocation budget.

(2) Implementation details

SQZ is provided as a C single header file only, to use it just define the macro
`SQZ_IMPLEMENTATION` in one file before including it:

#define SQZ_IMPLEMENTATION
#include "sqz.h"

No floating-point code is used in SQZ, the output is fully deterministic even when
using the non-reversible color modes.

To keep memory usage moderate, the DWT coefficients use 16 bits of precision, the
list nodes use 32-bit indexes for linking instead of pointers, and the lists are
only initialized on the first bitplane pass of their subband.

(3) License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#pragma once
#ifndef SQZ_H
#define SQZ_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    SQZ_RESULT_OK,                              /*!< The operation was completed successfully */
    SQZ_OUT_OF_MEMORY = -1,                     /*!< Not enough memory to perform the requested operation */
    SQZ_INVALID_PARAMETER = -2,                 /*!< An invalid parameter was sent */
    SQZ_BUFFER_TOO_SMALL = -3,                  /*!< The provided buffer was too small */
    SQZ_DATA_CORRUPTED = -4                     /*!< The compressed image data was corrupted */
} SQZ_status_t;

typedef enum
{
    SQZ_COLOR_MODE_GRAYSCALE,                   /*!< 8bpp grayscale mode */
    SQZ_COLOR_MODE_YCOCG_R,                     /*!< YCoCg-R colorspace mode */
    SQZ_COLOR_MODE_OKLAB,                       /*!< Oklab perceptual colorspace mode */
    SQZ_COLOR_MODE_LOG_L1,                      /*!< logl1 colorspace mode */
    SQZ_COLOR_MODE_COUNT,                       /*!< Number of color modes supported */
} SQZ_color_mode_t;

typedef enum
{
    SQZ_SCAN_ORDER_RASTER,                      /*!< Raster scan order */
    SQZ_SCAN_ORDER_SNAKE,                       /*!< Snake scan order*/
    SQZ_SCAN_ORDER_MORTON,                      /*!< Morton scan order */
    SQZ_SCAN_ORDER_HILBERT,                     /*!< Hilbert scan order */
    SQZ_SCAN_ORDER_COUNT,                       /*!< Number of scan orders supported */
} SQZ_scan_order_t;

/**
 * \brief           Maximum number of recursive spatial decompositions using the DWT
 * \hideinitializer
 */
#define SQZ_DWT_MAX_LEVEL   8

/**
 * \brief           Smallest spatial dimension supported
 * \hideinitializer
 */
#define SQZ_MIN_DIMENSION   8

/**
 * \brief           Highest spatial dimension supported
 * \hideinitializer
 */
#define SQZ_MAX_DIMENSION   ((1u << 16u) - 1u)

/**
 * \brief           Magic byte for SQZ image header
 * \hideinitializer
 */
#define SQZ_HEADER_MAGIC    0xA5

/**
 * \brief           SQZ image header size (in bytes)
 * \hideinitializer
 */
#define SQZ_HEADER_SIZE     6

/**
 * \brief           Structure used to describe an image
 * \note            When encoding, there is no need specifiy the number of planes
 */
typedef struct
{
    SQZ_color_mode_t color_mode;
    SQZ_scan_order_t scan_order;
    size_t width;
    size_t height;
    size_t dwt_levels;                          /*!< Number of DWT decomposition levels used */
    size_t num_planes;                          /*!< Number of spectral planes in the image */
    int subsampling;                            /*!< Specifies whether additional chroma subsampling is to be performed */
} SQZ_image_descriptor_t;

/**
 * \brief           Encode an image
 * \warning         The destination buffer will NOT be cleared before encoding
 * \param[in]       source : Pointer to the input pixel data
 * \param[out]      dest : Pointer to the buffer that will receive the compressed data, of at least `budget` bytes in size
 * \param[in,out]   descriptor : Pointer to an image descriptor, holding information about the image. Will be corrected if necessary
 * \param[in,out]   budget : Pointer to the byte budget allowed for compression, will be updated with the final compressed data size
 * \return          \ref SQZ_RESULT_OK on success, member of \ref SQZ_status_t otherwise
 */
SQZ_status_t SQZ_encode(void* const source, void* const dest, SQZ_image_descriptor_t* const descriptor, size_t* const budget);

/**
 * \brief           Decode an image
 * \note            Call this function with `dest_size` set to 0 to receive an image descriptor and the required buffer size,
 *                  if the return result is \ref SQZ_BUFFER_TOO_SMALL, any other result will imply a decoding error
 * \param[in]       source : Pointer to the input compressed data
 * \param[out]      dest : Pointer to the buffer that will receive the decompressed pixel data
 * \param[in]       src_size: Pointer to the size of the input buffer
 * \param[in,out]   dest_size : Pointer to the size of the output buffer (or 0 to request the appropriate size)
 * \param[in,out]   descriptor : Pointer to an image descriptor, to be filled with information about the image
 * \return          \ref SQZ_RESULT_OK on success, member of \ref SQZ_status_t otherwise
 */
SQZ_status_t SQZ_decode(void* const source, void* const dest, size_t const src_size, size_t* const dest_size, SQZ_image_descriptor_t* const descriptor);

#ifdef __cplusplus
}
#endif

#endif /* SQZ_H */

#ifdef SQZ_IMPLEMENTATION

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#define __restrict__ __restrict
#endif
#define restrict __restrict__

#if defined(CHAR_BIT) && (CHAR_BIT != 8)
#error  "Unsupported platform"
#endif

/**
 * \brief           Structure used for bitwise memory IO
 */
typedef struct
{
    uint8_t* data;                              /*!< Pointer to the initial position in the buffer */
    uint8_t* ptr;                               /*!< Pointer to the current byte being used in the buffer */
    uint8_t* eob;                               /*!< End-of-buffer pointer */
    size_t index;                               /*!< Index of the next bit available in the current byte */
} SQZ_bit_buffer_t;

/**
 * \brief           Node structure containing 2d coordinates relative to a subband, for a single-linked list
 */
typedef struct
{
    uint16_t x;                                 /*!< Horizontal coordinate relative to the origin of the subband */
    uint16_t y;                                 /*!< Vertical coordinate relative to the origin of the subband */
    int32_t next;                               /*!< Pointer to the next node in the list */
} SQZ_list_node_t;

/**
 * \brief           Structure containing the pre-allocated node cache used by all the lists in a subband
 */
typedef struct
{
    SQZ_list_node_t* nodes;                     /*!< Dynamic array of list nodes */
    size_t capacity;                            /*!< Number of nodes contained in this cache (equal to the number of coefficients in the subband) */
    size_t index;                               /*!< Index of the first available node */
} SQZ_list_node_cache_t;

/**
 * \brief           Single-linked list containing relative positions of coefficients from a subband
 */
typedef struct
{
    SQZ_list_node_cache_t* cache;               /*!< Cache where the list nodes are stored */
    SQZ_list_node_t* head;                      /*!< Pointer to the first element in the list */
    SQZ_list_node_t* tail;                      /*!< Pointer to the last element in the list */
    size_t length;
} SQZ_list_t;

typedef struct SQZ_scan_context SQZ_scan_context_t;
typedef int (*SQZ_scan_fn)(SQZ_scan_context_t* const ctx);
/**
 * \brief           Generic structure containing the common fields required for every scan order
 */
struct SQZ_scan_context
{
    void* workspace;                            /*!< Pointer to a structure holding the internal fields required for this scan order */
    SQZ_scan_fn scan;                           /*!< Pointer to the spatial traverse function for this scan order */
    SQZ_scan_order_t type;                      /*!< Type of scan order used in this context */
    size_t x;                                   /*!< Current relative horizontal coordinate */
    size_t y;                                   /*!< Current relative vertical coordinate */
    size_t width;                               /*!< Width covered by this scan order */
    size_t height;                              /*!< Height covered by this scan order */
};

/**
 * \brief           Structure containing the private fields required for the snake scan order
 * \note            This scan order divides the region to traverse into a grid of tiles, and
 *                  proceeds in an alternating horizontal and vertical direction, both inside
 *                  each tile and between tiles in the grid. It is the only scan order that
 *                  ensures that the Manhattan distance between sucessive moves is exactly 1
 */
typedef struct
{
    struct
    {
        size_t x;
        size_t y;
        size_t width;
        size_t height;
        struct
        {
            size_t remaining;
            int right_to_left;
        } columns;
        struct
        {
            size_t remaining;
        } rows;
        struct
        {
            size_t width;
            size_t height;
        } defaults;
    } tile;
    struct
    {
        size_t x;
        size_t y;
        size_t width;
        size_t height;
        struct
        {
            size_t index;
            int odd;
        } columns;
        struct
        {
            int odd;
        } rows;
    } grid;
    struct
    {
        size_t x;
        size_t y;
    } offsets;
} SQZ_snake_scan_context_t;

/**
 * \brief           Structure containing the private fields required for the Morton scan order
 * \note            Also known as Z-order, the coordinates are calculated by binary deinterleaving
 *                  of the current linear index
 */
typedef struct
{
    size_t range;
    size_t mask;
    size_t index;
    size_t length;
} SQZ_morton_scan_context_t;

/**
 * \brief           Stack item for the Hilbert scan order
 */
typedef struct
{
    int32_t x;
    int32_t y;
    int32_t ax;
    int32_t ay;
    int32_t bx;
    int32_t by;
} SQZ_hilbert_scan_stack_item_t;

/**
 * \brief           Stack containing the recursive region sub-division information for the Hilbert scan order
 */
typedef struct
{
    SQZ_hilbert_scan_stack_item_t items[32];
    int32_t index;
} SQZ_hilbert_scan_stack_t;

/**
 * \brief           Structure containing the private fields required for the Hilbert scan order
 * \note            Based on "Generalized Hilbert space-filling curve for rectangular domains of arbitrary
 *                  (non-power of two) sizes" - by Jakub Červený - [https://github.com/jakubcerveny/gilbert]
 */
typedef struct
{
    SQZ_hilbert_scan_stack_t stack;
    int32_t width;
    int32_t height;
    int32_t dax;
    int32_t day;
    int32_t dbx;
    int32_t dby;
    int32_t index;
} SQZ_hilbert_scan_context_t;

/**
 * \brief           Number of spectral planes supported
 * \hideinitializer
 */
#define SQZ_SPECTRAL_PLANES 3

/**
 * \brief           Number of subbands output by the DWT per iteration
 * \hideinitializer
 */
#define SQZ_DWT_SUBBANDS    4

typedef int16_t SQZ_dwt_coefficient_t;

/**
 * \brief           Structure used to describe a DWT subband
 */
typedef struct SQZ_dwt_subband
{
    SQZ_list_node_cache_t cache;                /*!< Common node cache shared by the lists, pre-allocated on first use */
    SQZ_list_t LIP;                             /*!< List of Insignificant Pixels */
    SQZ_list_t LSP;                             /*!< List of Significant Pixels */
    SQZ_list_t NSP;                             /*!< List of New Significant Pixels */
    SQZ_dwt_coefficient_t* data;                /*!< Pointer to the buffer holding the DWT coefficients for this subband */
    size_t width;                               /*!< Width of this subband */
    size_t height;                              /*!< Height of this subband */
    size_t stride;                              /*!< Stride size, in number of coefficients, between the lines of this subband in the buffer*/
    int max_bitplane;                           /*!< Highest bitplane in this subband containing at least one coefficient */
    int bitplane;                               /*!< Current bitplane being processed */
    int round;                                  /*!< Starting round for scheduling processing of this subband */
} SQZ_dwt_subband_t;

/**
 * \brief           Structure used to describe a spectral image plane
 */
typedef struct
{
    SQZ_dwt_subband_t band[SQZ_DWT_MAX_LEVEL][SQZ_DWT_SUBBANDS];    /*!< DWT subband tree for this plane */
    SQZ_dwt_coefficient_t* data;                /*!< Pointer to the buffer holding the pixel data for this plane */
} SQZ_spectral_plane_t;

/**
 * \brief           Structure used to store the codec internal state
 */
typedef struct
{
    SQZ_spectral_plane_t plane[SQZ_SPECTRAL_PLANES];    /*!< Spectral planes for this image*/
    SQZ_dwt_coefficient_t* data;                /*!< Pointer to the buffer holding the pixel data for this image */
    SQZ_bit_buffer_t buffer;                    /*!< I/O bit-wise buffer storing the compressed data */
    SQZ_image_descriptor_t image;               /*!< Image descriptor holding the relevant image information */
} SQZ_context_t;

typedef SQZ_status_t (*SQZ_init_subband_fn)(SQZ_dwt_subband_t* const band, SQZ_scan_context_t* const scan_ctx, SQZ_bit_buffer_t* const buffer);

typedef int (*SQZ_bitplane_task_fn)(SQZ_dwt_subband_t* const band, SQZ_bit_buffer_t* const buffer);

static uint8_t const SQZ_number_of_planes[SQZ_COLOR_MODE_COUNT] = { 1u, 3u, 3u, 3u, };

/**
 * \brief           Codec processing schedule defining the starting rounds for each subband, per level, plane and color mode
 */
static uint8_t const SQZ_schedule[SQZ_COLOR_MODE_COUNT][SQZ_SPECTRAL_PLANES][SQZ_DWT_MAX_LEVEL][SQZ_DWT_SUBBANDS] =
{
    /* Grayscale */
    {
        {
            {  0,  1,  1,  2, },
            {  0,  2,  2,  3, },
            {  0,  3,  3,  4, },
            {  0,  4,  4,  5, },
            {  0,  5,  5,  6, },
            {  0,  6,  6,  7, },
            {  0,  7,  7,  8, },
            {  0,  8,  8,  9, },
        },
        { { 0 } },
        { { 0 } },
    },
    /* YCoCg-R */
    {
        {
            {  0,  1,  1,  2, },
            {  0,  2,  2,  3, },
            {  0,  3,  3,  4, },
            {  0,  4,  4,  5, },
            {  0,  5,  5,  6, },
            {  0,  6,  6,  7, },
            {  0,  7,  7,  8, },
            {  0,  8,  8,  9, },
        },
        {
            {  1,  2,  2,  3, },
            {  0,  3,  3,  4, },
            {  0,  4,  4,  5, },
            {  0,  5,  5,  6, },
            {  0,  6,  6,  7, },
            {  0,  7,  7,  8, },
            {  0,  8,  8,  9, },
            {  0,  9,  9, 10, },
        },
        {
            {  1,  2,  2,  3, },
            {  0,  3,  3,  4, },
            {  0,  4,  4,  5, },
            {  0,  5,  5,  6, },
            {  0,  6,  6,  7, },
            {  0,  7,  7,  8, },
            {  0,  8,  8,  9, },
            {  0,  9,  9, 10, },
        },
    },
    /* Oklab */
    {
        {
            {  0,  1,  1,  2, },
            {  0,  2,  2,  3, },
            {  0,  3,  3,  4, },
            {  0,  4,  4,  5, },
            {  0,  5,  5,  6, },
            {  0,  6,  6,  7, },
            {  0,  7,  7,  8, },
            {  0,  8,  8,  9, },
        },
        {
            {  1,  2,  2,  3, },
            {  0,  3,  3,  4, },
            {  0,  4,  4,  5, },
            {  0,  5,  5,  6, },
            {  0,  6,  6,  7, },
            {  0,  7,  7,  8, },
            {  0,  8,  8,  9, },
            {  0,  9,  9, 10, },
        },
        {
            {  1,  2,  2,  3, },
            {  0,  3,  3,  4, },
            {  0,  4,  4,  5, },
            {  0,  5,  5,  6, },
            {  0,  6,  6,  7, },
            {  0,  7,  7,  8, },
            {  0,  8,  8,  9, },
            {  0,  9,  9, 10, },
        },
    },
    /* logl1 */
    {
        {
            {  0,  1,  1,  2, },
            {  0,  2,  2,  3, },
            {  0,  3,  3,  4, },
            {  0,  4,  4,  5, },
            {  0,  5,  5,  6, },
            {  0,  6,  6,  7, },
            {  0,  7,  7,  8, },
            {  0,  8,  8,  9, },
        },
        {
            {  1,  2,  2,  3, },
            {  0,  3,  3,  4, },
            {  0,  4,  4,  5, },
            {  0,  5,  5,  6, },
            {  0,  6,  6,  7, },
            {  0,  7,  7,  8, },
            {  0,  8,  8,  9, },
            {  0,  9,  9, 10, },
        },
        {
            {  1,  2,  2,  3, },
            {  0,  3,  3,  4, },
            {  0,  4,  4,  5, },
            {  0,  5,  5,  6, },
            {  0,  6,  6,  7, },
            {  0,  7,  7,  8, },
            {  0,  8,  8,  9, },
            {  0,  9,  9, 10, },
        },
    },
};

#undef SQZ_SPECTRAL_PLANES

#ifdef _MSC_VER
#include <intrin.h>
static uint32_t
SQZ_ilog2(uint32_t const x)
{
    unsigned long log = 0u;
    if (x != 0u)
    {
        _BitScanReverse(&log, x);
        ++log;
    };
    return log;
}
#elif defined(__GNUC__)
static uint32_t
SQZ_ilog2(uint32_t const x)
{
    return ((x != 0u) ? (uint32_t)(32 - __builtin_clz(x)) : x);
}
#endif

/**
 * \brief           Mirrors a value to the interval [0..maximum], used for symmetric extension at the image boundaries
 * \param           value: The signed value we wish to mirror
 * \param           maximum: Largest positive value inside the interval
 * \return          An unsigned integer inside the requested interval, mirrored around its boundaries
 */
static uint32_t
SQZ_mirror(int32_t value, int32_t const maximum)
{
    if (maximum == 0)
    {
        return 0u;
    }
    while ((uint32_t)value > (uint32_t)maximum)
    {
        value = -value;
        if (value < 0)
        {
            value += 2 * maximum;
        }
    }
    return (uint32_t)value;
}

/**
 * \brief           Interleaves the low 16-bits of a 32-bit unsigned integer to its even bits, clearing all the odd bits
 * \param           i: The value to interleave
 * \return          32-bit unsigned integer containing the low 16-bits of `i` interleaved into the even bits
 */
static uint32_t
SQZ_deinterleave_u32_to_u16(uint32_t i)
{
    i &= 0x55555555u;
    i = (i ^ (i >> 1u)) & 0x33333333u;
    i = (i ^ (i >> 2u)) & 0x0F0F0F0Fu;
    i = (i ^ (i >> 4u)) & 0x00FF00FFu;
    i = (i ^ (i >> 8u)) & 0x0000FFFFu;
    return i;
}

/**
 * \brief           Deinterleaves the even bits in a 32-bit unsigned integer
 * \param           i: The value to deinterleave
 * \return          32-bit unsigned integer containing the even bits of `i` packed in its low 16-bits
 */
static uint32_t
SQZ_interleave_u16_to_u32(uint32_t i)
{
    i &= 0x0000FFFFu;
    i = (i ^ (i << 8u)) & 0x00FF00FFu;
    i = (i ^ (i << 4u)) & 0x0F0F0F0Fu;
    i = (i ^ (i << 2u)) & 0x33333333u;
    i = (i ^ (i << 1u)) & 0x55555555u;
    return i;
}

static void
SQZ_bit_buffer_init(SQZ_bit_buffer_t* const buffer, void* const source, size_t const capacity)
{
#ifdef DEBUG
    if ((buffer == NULL) || (source == NULL) || (capacity == 0u))
    {
        return;
    }
#endif
    buffer->data = buffer->ptr = (uint8_t*)source;
    buffer->eob = buffer->data + capacity;
    buffer->index = 0u;
}

static int
SQZ_bit_buffer_eob(SQZ_bit_buffer_t const * const buffer)
{
#ifdef DEBUG
    if (buffer == NULL)
    {
        return 1;
    }
#endif
    return (buffer->ptr >= buffer->eob);
}

static size_t
SQZ_bit_buffer_bits_used(SQZ_bit_buffer_t const * const buffer)
{
#ifdef DEBUG
    if (buffer == NULL)
    {
        return 0;
    }
#endif
    return ((buffer->ptr - buffer->data) * CHAR_BIT) + buffer->index;
}

#define SQZ_BIT_BUFFER_MSB  ((sizeof(uint8_t) * CHAR_BIT) - 1u)

static int
SQZ_bit_buffer_write_bit(SQZ_bit_buffer_t* const buffer, uint32_t const bit)
{
#ifdef DEBUG
    if (buffer == NULL)
    {
        return 0;
    }
#endif
    if (SQZ_bit_buffer_eob(buffer))
    {
        return 0;
    }
    *(buffer->ptr) |= bit << (SQZ_BIT_BUFFER_MSB - buffer->index);
    if (buffer->index < SQZ_BIT_BUFFER_MSB)
    {
        buffer->index++;
    }
    else
    {
        buffer->ptr++;
        buffer->index = 0u;
    }
    return 1;
}

static int
SQZ_bit_buffer_write_bits(SQZ_bit_buffer_t* const buffer, uint32_t const bits, uint32_t width)
{
#ifdef DEBUG
    if (buffer == NULL)
    {
        return 0;
    }
#endif
    do
    {
        if (SQZ_bit_buffer_eob(buffer))
        {
            return 0;
        }
        uint32_t const bits_free = (SQZ_BIT_BUFFER_MSB + 1u) - buffer->index;
        if (bits_free >= width)
        {
            *(buffer->ptr) |= (bits & ((1u << width) - 1u)) << (bits_free - width);
            buffer->index += width;
            if (buffer->index > SQZ_BIT_BUFFER_MSB)
            {
                buffer->ptr++;
                buffer->index = 0u;
            }
            return 1;
        }
        else
        {
            *(buffer->ptr) |= (bits >> (width - bits_free)) & ((1u << bits_free) - 1u);
            buffer->ptr++;
            buffer->index = 0u;
            width -= bits_free;
        }
    }
    while (1);
}

static int32_t
SQZ_bit_buffer_read_bit(SQZ_bit_buffer_t* const buffer)
{
#ifdef DEBUG
    if (buffer == NULL)
    {
        return -1;
    }
#endif
    if (SQZ_bit_buffer_eob(buffer))
    {
        return -1;
    }
    int32_t const bit = (*(buffer->ptr) >> (SQZ_BIT_BUFFER_MSB - buffer->index)) & 1;
    if (buffer->index < SQZ_BIT_BUFFER_MSB)
    {
        buffer->index++;
    }
    else
    {
        buffer->ptr++;
        buffer->index = 0u;
    }
    return bit;
}

static int32_t
SQZ_bit_buffer_read_bits(SQZ_bit_buffer_t* const buffer, uint32_t width)
{
#ifdef DEBUG
    if (buffer == NULL)
    {
        return -1;
    }
#endif
    int32_t bits = 0;
    do
    {
        if (SQZ_bit_buffer_eob(buffer))
        {
            return -1;
        }
        uint32_t const bits_available = (SQZ_BIT_BUFFER_MSB + 1u) - buffer->index;
        if (bits_available >= width)
        {
            bits <<= width;
            bits |= (*(buffer->ptr) >> (bits_available - width)) & ((1u << width) - 1u);
            buffer->index += width;
            if (buffer->index > SQZ_BIT_BUFFER_MSB)
            {
                buffer->ptr++;
                buffer->index = 0u;
            }
            break;
        }
        else
        {
            bits <<= bits_available;
            bits |= *(buffer->ptr) & ((1u << bits_available) - 1u);
            buffer->ptr++;
            buffer->index = 0u;
            width -= bits_available;
        }
    }
    while (1);
    return bits;
}

#undef SQZ_BIT_BUFFER_MSB

static SQZ_status_t
SQZ_node_cache_init(SQZ_list_node_cache_t* const cache, size_t const capacity)
{
#ifdef DEBUG
    if ((cache == NULL) || (capacity == 0u))
    {
        return SQZ_INVALID_PARAMETER;
    }
#endif
    cache->nodes = (SQZ_list_node_t*)calloc(capacity, sizeof(SQZ_list_node_t));
    if (cache->nodes == NULL)
    {
        return SQZ_OUT_OF_MEMORY;
    }
    cache->capacity = capacity;
    cache->index = 0u;
    return SQZ_RESULT_OK;
}

#define SQZ_LIST_NULL -1

static void
SQZ_list_init(SQZ_list_t* const list, SQZ_list_node_cache_t* const cache)
{
#ifdef DEBUG
    if ((list == NULL) || (cache == NULL))
    {
        return;
    }
#endif
    list->cache = cache;
    list->head = list->tail = NULL;
    list->length = 0u;
}

static SQZ_list_node_t*
SQZ_list_node_next(SQZ_list_node_t const * const node, SQZ_list_node_t* const base)
{
#ifdef DEBUG
    return ((node != NULL) && (node->next > SQZ_LIST_NULL) && (base != NULL)) ? base + node->next : NULL;
#else
    return (node->next > SQZ_LIST_NULL) ? base + node->next : NULL;
#endif
}

static SQZ_list_node_t*
SQZ_list_add(SQZ_list_t* const list, uint16_t const x, uint16_t const y)
{
#ifdef DEBUG
    if (list == NULL)
    {
        return NULL;
    }
#endif
    SQZ_list_node_cache_t* const cache = list->cache;
    if (cache->index >= cache->capacity)
    {
        return NULL;
    }
    SQZ_list_node_t* const node = cache->nodes + cache->index;
    if (list->head == NULL)
    {
        list->head = node;
    }
    else if (list->tail != NULL)
    {
        list->tail->next = cache->index;
    }
    list->tail = node;
    list->length++;
    node->x = x;
    node->y = y;
    node->next = SQZ_LIST_NULL;
    cache->index++;
    return node;
}

/**
 * \brief           Exchanges a node from the source list to the destination list
 * \warning         Assumes that neither the list pointers nor the exchanged node pointer are `NULL`,
 *                  and that both lists share the same node cache but are not the same
 * \param[in,out]   source: Source list
 * \param[in,out]   dest: Destination list
 * \param[in,out]   node: Node from the source list to be exchanged to the destination list
 * \param[in,out]   prv: Previous node in the source list
 * \return          Pointer to the next node in the source list if available, `NULL` otherwise
*/
static SQZ_list_node_t*
SQZ_list_exchange(SQZ_list_t* restrict const source, SQZ_list_t* restrict const dest, SQZ_list_node_t* restrict const node, SQZ_list_node_t* restrict const prv)
{
#ifdef DEBUG
    if ((source == NULL) || (dest == NULL) || (source == dest) || (source->cache != dest->cache) || (node == NULL))
    {
        return NULL;
    }
#endif
    SQZ_list_node_t* const base = source->cache->nodes;
    SQZ_list_node_t* const next = SQZ_list_node_next(node, base);
    if (prv != NULL)
    {
        prv->next = node->next;
    }
    else
    {
        source->head = next;
    }
    source->length--;
    if (dest->head == NULL)
    {
        dest->head = node;
    }
    else if (dest->tail != NULL)
    {
        dest->tail->next = node - base;
    }
    dest->tail = node;
    dest->length++;
    node->next = SQZ_LIST_NULL;
    return next;
}

/**
 * \brief           Merges the source list into the destination list, clearing the source list
 * \warning         Assumes that neither of the list pointers are `NULL`, and that both lists
 *                  share the same node cache but are not the same
 * \param[in,out]   source: Source list
 * \param[in,out]   dest: Destination list
 */
static void
SQZ_list_merge(SQZ_list_t* const source, SQZ_list_t* const dest)
{
#ifdef DEBUG
    if ((source == NULL) || (dest == NULL) || (source == dest) || (source->cache != dest->cache))
    {
        return;
    }
#endif
    if (source->head == NULL)                   /* source list is empty, nothing to do */
    {
        return;
    }
    else if (dest->tail != NULL)                /* both lists have elements, append source to destination  */
    {
        dest->tail->next = source->head - source->cache->nodes;
    }
    else                                        /* destination list was empty, copy the source list to the head of the destination list */
    {
        dest->head = source->head;
    }
    dest->tail = source->tail;
    dest->length += source->length;
    source->length = 0u;
    source->head = source->tail = NULL;
}

#undef SQZ_LIST_NULL

static int
SQZ_scan_raster(SQZ_scan_context_t* const ctx)
{
#ifdef DEBUG
    if (ctx == NULL)
    {
        return 0;
    }
#endif
    ++ctx->x;
    if (ctx->x >= ctx->width)
    {
        ctx->x = 0u;
        ++ctx->y;
        if (ctx->y >= ctx->height)
        {
            return 0;
        }
    }
    return 1;
}

static SQZ_status_t
SQZ_scan_init_raster_context(SQZ_scan_context_t* const ctx, size_t const width, size_t const height)
{
#ifdef DEBUG
    if ((ctx == NULL) || (width == 0u) || (height == 0u))
    {
        return SQZ_INVALID_PARAMETER;
    }
#endif
    if (ctx->type != SQZ_SCAN_ORDER_RASTER)
    {
        ctx->type = SQZ_SCAN_ORDER_RASTER;
    }
    ctx->scan = SQZ_scan_raster;
    ctx->x = ctx->y = 0u;
    ctx->width = width;
    ctx->height = height;
    return SQZ_RESULT_OK;
}

static int
SQZ_scan_snake(SQZ_scan_context_t* const ctx)
{
#ifdef DEBUG
    if ((ctx == NULL) || (ctx->workspace == NULL))
    {
        return 0;
    }
#endif
    SQZ_snake_scan_context_t* const snake = (SQZ_snake_scan_context_t*)ctx->workspace;
    ++snake->tile.x;
    if (snake->tile.x < snake->tile.width)
    {
loop_tile_columns:
        ctx->x = ((snake->tile.columns.right_to_left) ? (snake->tile.width - 1u) - snake->tile.x : snake->tile.x) + snake->offsets.x;
        ctx->y = ((snake->grid.columns.odd) ? (snake->tile.height - 1u) - snake->tile.y : snake->tile.y) + snake->offsets.y;
    }
    else
    {
        snake->tile.x = 0u;
        ++snake->tile.y;
        /* can we move to next line in this tile? */
        if (snake->tile.y < snake->tile.height)
        {
loop_tile_rows:
            ;
            size_t const row = (snake->grid.columns.odd) ? (snake->tile.height - 1u) - snake->tile.y : snake->tile.y;
            snake->tile.columns.right_to_left = (snake->grid.y ^ row) & 1u;
            goto loop_tile_columns;
        }
        else
        {
            snake->tile.y = 0u;
            ++snake->grid.columns.index;
            /* can we move to next tile in this line of the grid? */
            if (snake->grid.columns.index < snake->grid.width)
            {
loop_grid_columns:
                ;
                size_t const width = snake->grid.width - 1u;
                snake->grid.x = (snake->grid.rows.odd) ? width - snake->grid.columns.index : snake->grid.columns.index;
                snake->grid.columns.odd = snake->grid.x & 1u;
                snake->tile.width = (snake->grid.x < width) ? snake->tile.defaults.width : snake->tile.columns.remaining;
                snake->offsets.x = snake->grid.x * snake->tile.defaults.width;
                goto loop_tile_rows;
            }
            else
            {
                snake->grid.columns.index = 0u;
                ++snake->grid.y;
                /* can we move to the next line of tiles in the grid ? */
                if (snake->grid.y < snake->grid.height)
                {
                    snake->grid.rows.odd = snake->grid.y & 1u;
                    snake->tile.height = (snake->grid.y < snake->grid.height - 1u) ? snake->tile.defaults.height : snake->tile.rows.remaining;
                    snake->offsets.y = snake->grid.y * snake->tile.defaults.height;
                    goto loop_grid_columns;
                }
                return 0;
            }
        }
    }
    return 1;
}

static SQZ_status_t
SQZ_scan_init_snake_context(SQZ_scan_context_t* const ctx, size_t width, size_t height, size_t tile_width, size_t tile_height)
{
#ifdef DEBUG
    if ((ctx == NULL) || (width == 0u) || (height == 0u) || (tile_width == 0u) || (tile_height == 0u))
    {
        return SQZ_INVALID_PARAMETER;
    }
#endif
    if ((ctx->type != SQZ_SCAN_ORDER_SNAKE) || (ctx->workspace == NULL))
    {
        ctx->type = SQZ_SCAN_ORDER_SNAKE;
        free(ctx->workspace);
        ctx->workspace = malloc(sizeof(SQZ_snake_scan_context_t));
        if (ctx->workspace == NULL)
        {
            return SQZ_OUT_OF_MEMORY;
        }
    }
    SQZ_snake_scan_context_t* const snake = (SQZ_snake_scan_context_t*)ctx->workspace;
    memset(snake, 0, sizeof(*snake));
    /* sanitize inputs */
    if (tile_width > width)
    {
        tile_width = width;
    }
    if (tile_height > height)
    {
        tile_height = height;
    }
    /* ensure that the grid has an odd number of columns */
    int32_t step = 1;
    for (;;)
    {
        snake->grid.width = (width + tile_width - 1u) / tile_width;
        if (!(snake->grid.width & 1u))
        {
            tile_width += step;
            if (tile_width > width)
                tile_width = width;
            else if (tile_width == 0u)
                tile_width = 1u;
            step = -(abs(step) + 1) * ((step > 0) - (step < 0));
        }
        else
        {
            break;
        }
    }
    snake->tile.columns.remaining = width % tile_width;
    if (snake->tile.columns.remaining == 0u)
    {
        snake->tile.columns.remaining = tile_width;
    }
    snake->tile.width = ((snake->grid.width > 1u) || (snake->tile.columns.remaining > 0u)) ? tile_width : snake->tile.columns.remaining;
    snake->tile.defaults.width = tile_width;
    /* ensure that for the last row of the grid, the tiles have an odd number of rows */
    step = 2;
    for (;;)
    {
        snake->tile.rows.remaining = height % tile_height;
        if ((snake->tile.rows.remaining > 0u) && !(snake->tile.rows.remaining & 1u))
        {
            tile_height += step;
            if (tile_height > height)
                tile_height = height;
            else if (tile_height == 0u)
                tile_height = 1u;
            step = -(abs(step) + 2) * ((step > 0) - (step < 0));
        }
        else
        {
            if (snake->tile.rows.remaining == 0u)
            {
                snake->tile.rows.remaining = tile_height;
            }
            break;
        }
    }
    snake->grid.height = (height + tile_height - 1u) / tile_height;
    snake->tile.height = ((snake->grid.height > 1u) || (snake->tile.rows.remaining > 0u)) ? tile_height : snake->tile.rows.remaining;
    snake->tile.defaults.height = tile_height;
    ctx->scan = SQZ_scan_snake;
    ctx->x = ctx->y = 0u;
    return SQZ_RESULT_OK;
}

static int
SQZ_scan_morton(SQZ_scan_context_t* const ctx)
{
#ifdef DEBUG
    if (ctx == NULL)
    {
        return 0;
    }
#endif
    SQZ_morton_scan_context_t* const morton = (SQZ_morton_scan_context_t*)ctx->workspace;
    size_t const length = morton->length, mask = morton->mask, range = morton->range;
    size_t const width = ctx->width, height = ctx->height;
    do
    {
        morton->index++;
        size_t const index = morton->index;
        ctx->x = SQZ_deinterleave_u32_to_u16(index & mask);
        ctx->y = SQZ_deinterleave_u32_to_u16((index >> 1u) & mask);
        uint32_t const m = (index & (~mask)) >> range;
        if (width > height)
        {
            ctx->x |= m;
        }
        else
        {
            ctx->y |= m;
        }
        if ((ctx->x < width) && (ctx->y < height))
        {
            return 1;
        }
    }
    while(morton->index < length);
    return 0;
}

static SQZ_status_t
SQZ_scan_init_morton_context(SQZ_scan_context_t* const ctx, size_t width, size_t height)
{
#ifdef DEBUG
    if ((ctx == NULL) || (width == 0u) || (height == 0u))
    {
        return SQZ_INVALID_PARAMETER;
    }
#endif
    if ((ctx->type != SQZ_SCAN_ORDER_MORTON) || (ctx->workspace == NULL))
    {
        ctx->type = SQZ_SCAN_ORDER_MORTON;
        free(ctx->workspace);
        ctx->workspace = malloc(sizeof(SQZ_morton_scan_context_t));
        if (ctx->workspace == NULL)
        {
            return SQZ_OUT_OF_MEMORY;
        }
    }
    SQZ_morton_scan_context_t* const morton = (SQZ_morton_scan_context_t*)ctx->workspace;
    memset(morton, 0, sizeof(*morton));
    morton->range = SQZ_ilog2(((width > height) ? height : width) - 1u);
    morton->mask = (size_t)((1ul << (morton->range * 2u)) - 1u);
    morton->length = (size_t)(1ul << (morton->range + SQZ_ilog2(((width > height) ? width : height) - 1u)));
    ctx->scan = SQZ_scan_morton;
    ctx->x = ctx->y = 0u;
    ctx->width = width;
    ctx->height = height;
    return SQZ_RESULT_OK;
}

static void
SQZ_scan_hilbert_stack_push(SQZ_hilbert_scan_stack_t* const stack, int32_t const x, int32_t const y, int32_t const ax, int32_t const ay, int32_t const bx, int32_t const by)
{
    SQZ_hilbert_scan_stack_item_t* const item = &stack->items[stack->index];
    item->x = x;
    item->y = y;
    item->ax = ax;
    item->ay = ay;
    item->bx = bx;
    item->by = by;
    stack->index++;
}

#define SQZ_sign(x) (((x) < 0) ? -1 : (((x) > 0) ? 1 : 0))

static int
SQZ_scan_hilbert(SQZ_scan_context_t* const ctx)
{
#ifdef DEBUG
    if (ctx == NULL)
    {
        return 0;
    }
#endif
    SQZ_hilbert_scan_context_t* const hilbert = (SQZ_hilbert_scan_context_t*)ctx->workspace;
    SQZ_hilbert_scan_stack_item_t* item;
loop:
    if (hilbert->stack.index == 0)
    {
        return 0;
    }
    item = &hilbert->stack.items[hilbert->stack.index - 1];
    if (hilbert->index < 0)
    {
        hilbert->width = abs(item->ax + item->ay);
        hilbert->height = abs(item->bx + item->by);
        hilbert->dax = SQZ_sign(item->ax);
        hilbert->day = SQZ_sign(item->ay);
        hilbert->dbx = SQZ_sign(item->bx);
        hilbert->dby = SQZ_sign(item->by);
        hilbert->index = 0;
    }
    if (hilbert->height == 1)
    {
        if (hilbert->index < hilbert->width)
        {
            ctx->x = item->x;
            ctx->y = item->y;
            item->x += hilbert->dax;
            item->y += hilbert->day;
            hilbert->index++;
            return 1;
        }
        else
        {
            hilbert->stack.index--;
            hilbert->index = -1;
            goto loop;
        }
    }
    if (hilbert->width == 1)
    {
        if (hilbert->index < hilbert->height)
        {
            ctx->x = item->x;
            ctx->y = item->y;
            item->x += hilbert->dbx;
            item->y += hilbert->dby;
            hilbert->index++;
            return 1;
        }
        else
        {
            hilbert->stack.index--;
            hilbert->index = -1;
            goto loop;
        }
    }
    SQZ_hilbert_scan_stack_item_t current;
    memcpy(&current, item, sizeof(current));
    hilbert->stack.index--;
    hilbert->index = -1;
    int32_t ax2 = current.ax / 2;
    int32_t ay2 = current.ay / 2;
    int32_t bx2 = current.bx / 2;
    int32_t by2 = current.by / 2;
    int32_t const w2 = abs(ax2 + ay2);
    int32_t const h2 = abs(bx2 + by2);
    if (2 * hilbert->width > 3 * hilbert->height)
    {
        if (((w2 % 2) != 0) && (hilbert->width > 2))
        {
            ax2 += hilbert->dax;
            ay2 += hilbert->day;
        }
        SQZ_scan_hilbert_stack_push(&hilbert->stack, current.x + ax2, current.y + ay2, current.ax - ax2, current.ay - ay2, current.bx, current.by);
        SQZ_scan_hilbert_stack_push(&hilbert->stack, current.x, current.y, ax2, ay2, current.bx, current.by);
    }
    else
    {
        if (((h2 % 2) != 0) && (hilbert->height > 2))
        {
            bx2 += hilbert->dbx;
            by2 += hilbert->dby;
        }
        SQZ_scan_hilbert_stack_push(&hilbert->stack, current.x + (current.ax - hilbert->dax) + (bx2 - hilbert->dbx), current.y + (current.ay - hilbert->day) + (by2 - hilbert->dby), -bx2, -by2, -(current.ax - ax2), -(current.ay - ay2));
        SQZ_scan_hilbert_stack_push(&hilbert->stack, current.x + bx2, current.y + by2, current.ax, current.ay, current.bx - bx2, current.by - by2);
        SQZ_scan_hilbert_stack_push(&hilbert->stack, current.x, current.y, bx2, by2, ax2, ay2);
    }
    goto loop;
}

static SQZ_status_t
SQZ_scan_init_hilbert_context(SQZ_scan_context_t* const ctx, size_t width, size_t height)
{
#ifdef DEBUG
    if ((ctx == NULL) || (width == 0u) || (height == 0u))
    {
        return SQZ_INVALID_PARAMETER;
    }
#endif
    if ((ctx->type != SQZ_SCAN_ORDER_HILBERT) || (ctx->workspace == NULL))
    {
        ctx->type = SQZ_SCAN_ORDER_HILBERT;
        free(ctx->workspace);
        ctx->workspace = malloc(sizeof(SQZ_hilbert_scan_context_t));
        if (ctx->workspace == NULL)
        {
            return SQZ_OUT_OF_MEMORY;
        }
    }
    SQZ_hilbert_scan_context_t* const hilbert = (SQZ_hilbert_scan_context_t*)ctx->workspace;
    memset(hilbert, 0, sizeof(*hilbert));
    if (width >= height)
    {
        SQZ_scan_hilbert_stack_push(&hilbert->stack, 0, 0, width, 0, 0, height);
    }
    else
    {
        SQZ_scan_hilbert_stack_push(&hilbert->stack, 0, 0, 0, height, width, 0);
    }
    hilbert->index = -1;
    SQZ_scan_hilbert(ctx);
    ctx->scan = SQZ_scan_hilbert;
    return SQZ_RESULT_OK;
}

#undef SQZ_SIGN

#define SQZ_SCAN_SNAKE_DEFAULT_TILE_WIDTH   4
#define SQZ_SCAN_SNAKE_DEFAULT_TILE_HEIGHT  15

static SQZ_status_t
SQZ_scan_init(SQZ_scan_context_t* const ctx, SQZ_dwt_subband_t const * const band)
{
#ifdef DEBUG
    if ((ctx == NULL) || (band == NULL))
    {
        return SQZ_INVALID_PARAMETER;
    }
#endif
    switch (ctx->type)
    {
    case SQZ_SCAN_ORDER_RASTER:
        return SQZ_scan_init_raster_context(ctx, band->width, band->height);
    case SQZ_SCAN_ORDER_SNAKE:
        return SQZ_scan_init_snake_context(ctx, band->width, band->height, SQZ_SCAN_SNAKE_DEFAULT_TILE_WIDTH, SQZ_SCAN_SNAKE_DEFAULT_TILE_HEIGHT);
    case SQZ_SCAN_ORDER_MORTON:
        return SQZ_scan_init_morton_context(ctx, band->width, band->height);
    case SQZ_SCAN_ORDER_HILBERT:
        return SQZ_scan_init_hilbert_context(ctx, band->width, band->height);
    default:
        return SQZ_INVALID_PARAMETER;
    }
}

#undef SQZ_SCAN_SNAKE_DEFAULT_TILE_WIDTH
#undef SQZ_SCAN_SNAKE_DEFAULT_TILE_HEIGHT

#define SQZ_COLOR_8BPC_LEVEL_OFFSET 128
#define SQZ_COLOR_CLIP(v) (((v) < 0) ? 0u : ((v) > 255 ? 255u : (uint8_t)(v)))

static void
SQZ_color_process_grayscale(SQZ_context_t* const ctx, void* const buffer, int const read)
{
#ifdef DEBUG
    if ((ctx == NULL) || (buffer == NULL))
    {
        return;
    }
#endif
    SQZ_dwt_coefficient_t* const data = ctx->data;
    uint8_t * const ptr = (uint8_t*)buffer;
    size_t const length = ctx->image.width * ctx->image.height;
    if (read)
    {
        for (size_t i = 0u; i < length; ++i)
        {
            data[i] = ((SQZ_dwt_coefficient_t)ptr[i]) - SQZ_COLOR_8BPC_LEVEL_OFFSET;
        }
    }
    else
    {
        for (size_t i = 0u; i < length; ++i)
        {
            SQZ_dwt_coefficient_t v = data[i] + SQZ_COLOR_8BPC_LEVEL_OFFSET;
            ptr[i] = (v < 0) ? 0u : (v > 255 ? 255u : (uint8_t)v);
        }
    }
}

/*
Based on "YCoCg-R: A Color Space with RGB Reversibility and Low Dynamic Range" - by Henrique Malvar
and Gary Sullivan - [https://wftp3.itu.int/av-arch/jvt-site/2003_09_SanDiego/JVT-I014r3.doc]
*/

static void
SQZ_color_process_ycocg_r(SQZ_context_t* const ctx, void* const buffer, int const read)
{
#ifdef DEBUG
    if ((ctx == NULL) || (buffer == NULL))
    {
        return;
    }
#endif
    SQZ_dwt_coefficient_t * restrict Y = ctx->plane[0].data, * restrict Co = ctx->plane[1].data, * restrict Cg = ctx->plane[2].data;
    uint8_t* ptr = (uint8_t*)buffer;
    size_t const length = ctx->image.width * ctx->image.height;
    if (read)
    {
        for (size_t i = 0u; i < length; ++i)
        {
            SQZ_dwt_coefficient_t const R = *ptr++, G = *ptr++, B = *ptr++, t = (R + B) >> 1;
            *Y++ = ((t + G) >> 1) - SQZ_COLOR_8BPC_LEVEL_OFFSET;
            *Co++ = R - B;
            *Cg++ = G - t;
        }
    }
    else
    {
        for (size_t i = 0u; i < length; ++i)
        {
            SQZ_dwt_coefficient_t const Y_ = (*Y++) + SQZ_COLOR_8BPC_LEVEL_OFFSET, Co_ = (*Co++), Cg_ = (*Cg++);
            SQZ_dwt_coefficient_t const B = Y_ + ((1 - Cg_) >> 1) - (Co_ >> 1);
            SQZ_dwt_coefficient_t const G = Y_ - ((-Cg_) >> 1);
            SQZ_dwt_coefficient_t const R = Co_ + B;
            *ptr++ = SQZ_COLOR_CLIP(R);
            *ptr++ = SQZ_COLOR_CLIP(G);
            *ptr++ = SQZ_COLOR_CLIP(B);
        }
    }
}

#undef SQZ_COLOR_8BPC_LEVEL_OFFSET

/*
Based on "Oklab - A perceptual color space for image processing" - by Björn Ottosson
                [https://bottosson.github.io/posts/oklab/]

Oklab <-> sRGB conversion is based on "Porting OkLab colorspace to integer arithmetic"
    [https://blog.pkh.me/p/38-porting-oklab-colorspace-to-integer-arithmetic.html]

Accuracy was traded-off for performance whenever needed, 12bpc are used for Oklab
*/

#define SQZ_COLOR_LINEAR_PRECISION          16
#define SQZ_COLOR_LINEAR_MAX                ((1 << SQZ_COLOR_LINEAR_PRECISION) - 1)
#define SQZ_COLOR_LINEAR_TO_SRGB_PRECISION  9
#define SQZ_COLOR_LINEAR_TO_SRGB_LUT_SIZE   ((1 << SQZ_COLOR_LINEAR_TO_SRGB_PRECISION) - 1)

static const uint16_t SQZ_sRGB_to_linear[256] =
{
    0x0000, 0x0014, 0x0028, 0x003C, 0x0050, 0x0063, 0x0077, 0x008B,
    0x009F, 0x00B3, 0x00C7, 0x00DB, 0x00F1, 0x0108, 0x0120, 0x0139,
    0x0154, 0x016F, 0x018C, 0x01AB, 0x01CA, 0x01EB, 0x020E, 0x0232,
    0x0257, 0x027D, 0x02A5, 0x02CE, 0x02F9, 0x0325, 0x0353, 0x0382,
    0x03B3, 0x03E5, 0x0418, 0x044D, 0x0484, 0x04BC, 0x04F6, 0x0532,
    0x056F, 0x05AD, 0x05ED, 0x062F, 0x0673, 0x06B8, 0x06FE, 0x0747,
    0x0791, 0x07DD, 0x082A, 0x087A, 0x08CA, 0x091D, 0x0972, 0x09C8,
    0x0A20, 0x0A79, 0x0AD5, 0x0B32, 0x0B91, 0x0BF2, 0x0C55, 0x0CBA,
    0x0D20, 0x0D88, 0x0DF2, 0x0E5E, 0x0ECC, 0x0F3C, 0x0FAE, 0x1021,
    0x1097, 0x110E, 0x1188, 0x1203, 0x1280, 0x1300, 0x1381, 0x1404,
    0x1489, 0x1510, 0x159A, 0x1625, 0x16B2, 0x1741, 0x17D3, 0x1866,
    0x18FB, 0x1993, 0x1A2C, 0x1AC8, 0x1B66, 0x1C06, 0x1CA7, 0x1D4C,
    0x1DF2, 0x1E9A, 0x1F44, 0x1FF1, 0x20A0, 0x2150, 0x2204, 0x22B9,
    0x2370, 0x242A, 0x24E5, 0x25A3, 0x2664, 0x2726, 0x27EB, 0x28B1,
    0x297B, 0x2A46, 0x2B14, 0x2BE3, 0x2CB6, 0x2D8A, 0x2E61, 0x2F3A,
    0x3015, 0x30F2, 0x31D2, 0x32B4, 0x3399, 0x3480, 0x3569, 0x3655,
    0x3742, 0x3833, 0x3925, 0x3A1A, 0x3B12, 0x3C0B, 0x3D07, 0x3E06,
    0x3F07, 0x400A, 0x4110, 0x4218, 0x4323, 0x4430, 0x453F, 0x4651,
    0x4765, 0x487C, 0x4995, 0x4AB1, 0x4BCF, 0x4CF0, 0x4E13, 0x4F39,
    0x5061, 0x518C, 0x52B9, 0x53E9, 0x551B, 0x5650, 0x5787, 0x58C1,
    0x59FE, 0x5B3D, 0x5C7E, 0x5DC2, 0x5F09, 0x6052, 0x619E, 0x62ED,
    0x643E, 0x6591, 0x66E8, 0x6840, 0x699C, 0x6AFA, 0x6C5B, 0x6DBE,
    0x6F24, 0x708D, 0x71F8, 0x7366, 0x74D7, 0x764A, 0x77C0, 0x7939,
    0x7AB4, 0x7C32, 0x7DB3, 0x7F37, 0x80BD, 0x8246, 0x83D1, 0x855F,
    0x86F0, 0x8884, 0x8A1B, 0x8BB4, 0x8D50, 0x8EEF, 0x9090, 0x9235,
    0x93DC, 0x9586, 0x9732, 0x98E2, 0x9A94, 0x9C49, 0x9E01, 0x9FBB,
    0xA179, 0xA339, 0xA4FC, 0xA6C2, 0xA88B, 0xAA56, 0xAC25, 0xADF6,
    0xAFCA, 0xB1A1, 0xB37B, 0xB557, 0xB737, 0xB919, 0xBAFF, 0xBCE7,
    0xBED2, 0xC0C0, 0xC2B1, 0xC4A5, 0xC69C, 0xC895, 0xCA92, 0xCC91,
    0xCE94, 0xD099, 0xD2A1, 0xD4AD, 0xD6BB, 0xD8CC, 0xDAE0, 0xDCF7,
    0xDF11, 0xE12E, 0xE34E, 0xE571, 0xE797, 0xE9C0, 0xEBEC, 0xEE1B,
    0xF04D, 0xF282, 0xF4BA, 0xF6F5, 0xF933, 0xFB74, 0xFDB8, 0xFFFF,
};

static const uint8_t SQZ_linear_to_sRGB[SQZ_COLOR_LINEAR_TO_SRGB_LUT_SIZE + 1] =
{
    0x00, 0x06, 0x0D, 0x12, 0x16, 0x19, 0x1C, 0x1F, 0x22, 0x24, 0x26, 0x28, 0x2A, 0x2C, 0x2E, 0x30,
    0x32, 0x33, 0x35, 0x36, 0x38, 0x39, 0x3B, 0x3C, 0x3D, 0x3E, 0x40, 0x41, 0x42, 0x43, 0x45, 0x46,
    0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56,
    0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x5F, 0x60, 0x61, 0x62, 0x62,
    0x63, 0x64, 0x65, 0x65, 0x66, 0x67, 0x67, 0x68, 0x69, 0x6A, 0x6A, 0x6B, 0x6C, 0x6C, 0x6D, 0x6E,
    0x6E, 0x6F, 0x6F, 0x70, 0x71, 0x71, 0x72, 0x73, 0x73, 0x74, 0x74, 0x75, 0x76, 0x76, 0x77, 0x77,
    0x78, 0x79, 0x79, 0x7A, 0x7A, 0x7B, 0x7B, 0x7C, 0x7D, 0x7D, 0x7E, 0x7E, 0x7F, 0x7F, 0x80, 0x80,
    0x81, 0x81, 0x82, 0x82, 0x83, 0x84, 0x84, 0x85, 0x85, 0x86, 0x86, 0x87, 0x87, 0x88, 0x88, 0x89,
    0x89, 0x8A, 0x8A, 0x8B, 0x8B, 0x8C, 0x8C, 0x8C, 0x8D, 0x8D, 0x8E, 0x8E, 0x8F, 0x8F, 0x90, 0x90,
    0x91, 0x91, 0x92, 0x92, 0x93, 0x93, 0x93, 0x94, 0x94, 0x95, 0x95, 0x96, 0x96, 0x97, 0x97, 0x97,
    0x98, 0x98, 0x99, 0x99, 0x9A, 0x9A, 0x9A, 0x9B, 0x9B, 0x9C, 0x9C, 0x9C, 0x9D, 0x9D, 0x9E, 0x9E,
    0x9F, 0x9F, 0x9F, 0xA0, 0xA0, 0xA1, 0xA1, 0xA1, 0xA2, 0xA2, 0xA3, 0xA3, 0xA3, 0xA4, 0xA4, 0xA5,
    0xA5, 0xA5, 0xA6, 0xA6, 0xA6, 0xA7, 0xA7, 0xA8, 0xA8, 0xA8, 0xA9, 0xA9, 0xA9, 0xAA, 0xAA, 0xAB,
    0xAB, 0xAB, 0xAC, 0xAC, 0xAC, 0xAD, 0xAD, 0xAE, 0xAE, 0xAE, 0xAF, 0xAF, 0xAF, 0xB0, 0xB0, 0xB0,
    0xB1, 0xB1, 0xB1, 0xB2, 0xB2, 0xB3, 0xB3, 0xB3, 0xB4, 0xB4, 0xB4, 0xB5, 0xB5, 0xB5, 0xB6, 0xB6,
    0xB6, 0xB7, 0xB7, 0xB7, 0xB8, 0xB8, 0xB8, 0xB9, 0xB9, 0xB9, 0xBA, 0xBA, 0xBA, 0xBB, 0xBB, 0xBB,
    0xBC, 0xBC, 0xBC, 0xBD, 0xBD, 0xBD, 0xBE, 0xBE, 0xBE, 0xBF, 0xBF, 0xBF, 0xC0, 0xC0, 0xC0, 0xC1,
    0xC1, 0xC1, 0xC1, 0xC2, 0xC2, 0xC2, 0xC3, 0xC3, 0xC3, 0xC4, 0xC4, 0xC4, 0xC5, 0xC5, 0xC5, 0xC6,
    0xC6, 0xC6, 0xC6, 0xC7, 0xC7, 0xC7, 0xC8, 0xC8, 0xC8, 0xC9, 0xC9, 0xC9, 0xC9, 0xCA, 0xCA, 0xCA,
    0xCB, 0xCB, 0xCB, 0xCC, 0xCC, 0xCC, 0xCC, 0xCD, 0xCD, 0xCD, 0xCE, 0xCE, 0xCE, 0xCE, 0xCF, 0xCF,
    0xCF, 0xD0, 0xD0, 0xD0, 0xD0, 0xD1, 0xD1, 0xD1, 0xD2, 0xD2, 0xD2, 0xD2, 0xD3, 0xD3, 0xD3, 0xD4,
    0xD4, 0xD4, 0xD4, 0xD5, 0xD5, 0xD5, 0xD6, 0xD6, 0xD6, 0xD6, 0xD7, 0xD7, 0xD7, 0xD7, 0xD8, 0xD8,
    0xD8, 0xD9, 0xD9, 0xD9, 0xD9, 0xDA, 0xDA, 0xDA, 0xDA, 0xDB, 0xDB, 0xDB, 0xDC, 0xDC, 0xDC, 0xDC,
    0xDD, 0xDD, 0xDD, 0xDD, 0xDE, 0xDE, 0xDE, 0xDE, 0xDF, 0xDF, 0xDF, 0xE0, 0xE0, 0xE0, 0xE0, 0xE1,
    0xE1, 0xE1, 0xE1, 0xE2, 0xE2, 0xE2, 0xE2, 0xE3, 0xE3, 0xE3, 0xE3, 0xE4, 0xE4, 0xE4, 0xE4, 0xE5,
    0xE5, 0xE5, 0xE5, 0xE6, 0xE6, 0xE6, 0xE6, 0xE7, 0xE7, 0xE7, 0xE7, 0xE8, 0xE8, 0xE8, 0xE8, 0xE9,
    0xE9, 0xE9, 0xE9, 0xEA, 0xEA, 0xEA, 0xEA, 0xEB, 0xEB, 0xEB, 0xEB, 0xEC, 0xEC, 0xEC, 0xEC, 0xED,
    0xED, 0xED, 0xED, 0xEE, 0xEE, 0xEE, 0xEE, 0xEF, 0xEF, 0xEF, 0xEF, 0xEF, 0xF0, 0xF0, 0xF0, 0xF0,
    0xF1, 0xF1, 0xF1, 0xF1, 0xF2, 0xF2, 0xF2, 0xF2, 0xF3, 0xF3, 0xF3, 0xF3, 0xF3, 0xF4, 0xF4, 0xF4,
    0xF4, 0xF5, 0xF5, 0xF5, 0xF5, 0xF6, 0xF6, 0xF6, 0xF6, 0xF6, 0xF7, 0xF7, 0xF7, 0xF7, 0xF8, 0xF8,
    0xF8, 0xF8, 0xF9, 0xF9, 0xF9, 0xF9, 0xF9, 0xFA, 0xFA, 0xFA, 0xFA, 0xFB, 0xFB, 0xFB, 0xFB, 0xFB,
    0xFC, 0xFC, 0xFC, 0xFC, 0xFD, 0xFD, 0xFD, 0xFD, 0xFD, 0xFE, 0xFE, 0xFE, 0xFE, 0xFF, 0xFF, 0xFF,
};

static uint8_t
SQZ_linear_i32_to_sRGB_u8(int32_t const v)
{
    if (v <= 0)
    {
        return 0;
    }
    if (v >= SQZ_COLOR_LINEAR_MAX)
    {
        return 0xFF;
    }
    int32_t const vmul = v * SQZ_COLOR_LINEAR_TO_SRGB_LUT_SIZE;
    int32_t const offset = vmul >> SQZ_COLOR_LINEAR_PRECISION;
    int32_t const interpoland = vmul & SQZ_COLOR_LINEAR_MAX;
    int32_t const base = SQZ_linear_to_sRGB[offset];
    return base + ((interpoland * (SQZ_linear_to_sRGB[offset + 1] - base)) >> SQZ_COLOR_LINEAR_PRECISION);
}

static int32_t
SQZ_i32_cbrt_01(int32_t const v)
{
    if (v <= 0)
    {
        return 0;
    }
    if (v >= SQZ_COLOR_LINEAR_MAX)
    {
        return SQZ_COLOR_LINEAR_MAX;
    }
    int64_t root = ((v * (((v * (v - 144107LL)) >> SQZ_COLOR_LINEAR_PRECISION) + 132114LL)) >> SQZ_COLOR_LINEAR_PRECISION) + 14379LL;
    for (int i = 0; i < 2; i++)
    {
        int64_t const n = root * root * root, denominator = v + (n >> (SQZ_COLOR_LINEAR_PRECISION * 2 - 1));
        root = (root * (2LL * v + (n >> (SQZ_COLOR_LINEAR_PRECISION * 2)))) / denominator;
    }
    return (int32_t)root;
}

#define SQZ_COLOR_OKLAB_PRECISION   12
#define SQZ_COLOR_OKLAB_MUL         (1LL << (SQZ_COLOR_LINEAR_PRECISION - SQZ_COLOR_OKLAB_PRECISION))
#define SQZ_COLOR_OKLAB_LEVEL_OFFSET (1 << (SQZ_COLOR_OKLAB_PRECISION - 1))

static void
SQZ_color_process_oklab(SQZ_context_t* const ctx, void* const buffer, int const read)
{
#ifdef DEBUG
    if ((ctx == NULL) || (buffer == NULL))
    {
        return;
    }
#endif
    SQZ_dwt_coefficient_t * restrict L = ctx->plane[0].data, * restrict a = ctx->plane[1].data, * restrict b = ctx->plane[2].data;
    uint8_t* ptr = (uint8_t*)buffer;
    size_t const length = ctx->image.width * ctx->image.height;
    if (read)
    {
        for (size_t i = 0u; i < length; ++i)
        {
            int32_t const R = (int32_t)SQZ_sRGB_to_linear[*ptr++];
            int32_t const G = (int32_t)SQZ_sRGB_to_linear[*ptr++];
            int32_t const B = (int32_t)SQZ_sRGB_to_linear[*ptr++];
            int32_t const l = SQZ_i32_cbrt_01((int32_t)((27015LL * R + 35149LL * G +  3372LL * B) >> SQZ_COLOR_LINEAR_PRECISION));
            int32_t const m = SQZ_i32_cbrt_01((int32_t)((13887LL * R + 44610LL * G +  7038LL * B) >> SQZ_COLOR_LINEAR_PRECISION));
            int32_t const s = SQZ_i32_cbrt_01((int32_t)(( 5787LL * R + 18462LL * G + 41286LL * B) >> SQZ_COLOR_LINEAR_PRECISION));
            *L++ =(( 862L * l + 3250L * m -   17L * s + (SQZ_COLOR_LINEAR_MAX / 2)) >> SQZ_COLOR_LINEAR_PRECISION) - SQZ_COLOR_OKLAB_LEVEL_OFFSET;
            *a++ = (8100L * l - 9945L * m + 1845L * s + (SQZ_COLOR_LINEAR_MAX / 2)) >> SQZ_COLOR_LINEAR_PRECISION;
            *b++ = ( 106L * l + 3205L * m - 3311L * s + (SQZ_COLOR_LINEAR_MAX / 2)) >> SQZ_COLOR_LINEAR_PRECISION;
        }
    }
    else
    {
        for (size_t i = 0u; i < length; ++i)
        {
            SQZ_dwt_coefficient_t const L_ = (*L++) + SQZ_COLOR_OKLAB_LEVEL_OFFSET, a_ = (*a++), b_ = (*b++);
            int64_t const l_ = L_ * SQZ_COLOR_OKLAB_MUL + ((25974LL * a_ + 14143LL * b_) >> SQZ_COLOR_OKLAB_PRECISION);
            int64_t const m_ = L_ * SQZ_COLOR_OKLAB_MUL + ((-6918LL * a_ -  4185LL * b_) >> SQZ_COLOR_OKLAB_PRECISION);
            int64_t const s_ = L_ * SQZ_COLOR_OKLAB_MUL + ((-5864LL * a_ - 84638LL * b_) >> SQZ_COLOR_OKLAB_PRECISION);
            int64_t const l = (l_ * l_ * l_) >> (SQZ_COLOR_LINEAR_PRECISION * 2);
            int64_t const m = (m_ * m_ * m_) >> (SQZ_COLOR_LINEAR_PRECISION * 2);
            int64_t const s = (s_ * s_ * s_) >> (SQZ_COLOR_LINEAR_PRECISION * 2);
            *ptr++ = SQZ_linear_i32_to_sRGB_u8((int32_t)((267169LL * l - 216771LL * m +  15137LL * s) >> SQZ_COLOR_LINEAR_PRECISION));
            *ptr++ = SQZ_linear_i32_to_sRGB_u8((int32_t)((-83127LL * l + 171030LL * m -  22368LL * s) >> SQZ_COLOR_LINEAR_PRECISION));
            *ptr++ = SQZ_linear_i32_to_sRGB_u8((int32_t)((  -275LL * l -  46099LL * m + 111909LL * s) >> SQZ_COLOR_LINEAR_PRECISION));
        }
    }
}

#undef SQZ_COLOR_OKLAB_PRECISION
#undef SQZ_COLOR_OKLAB_MUL
#undef SQZ_COLOR_OKLAB_LEVEL_OFFSET
#undef SQZ_COLOR_LINEAR_PRECISION
#undef SQZ_COLOR_LINEAR_MAX
#undef SQZ_COLOR_LINEAR_TO_SRGB_PRECISION
#undef SQZ_COLOR_LINEAR_TO_SRGB_LUT_SIZE

#define SQZ_COLOR_LOGL1_LEVEL_OFFSET    221

/*
Based on "Exploiting context dependence for image compression with upsampling" - by Jarek Duda
                            [https://arxiv.org/abs/2004.03391]
*/

static void
SQZ_color_process_logl1(SQZ_context_t* const ctx, void* const buffer, int const read)
{
#ifdef DEBUG
    if ((ctx == NULL) || (buffer == NULL))
    {
        return;
    }
#endif
    SQZ_dwt_coefficient_t * restrict Y = ctx->plane[0].data, * restrict c0 = ctx->plane[1].data, * restrict c1 = ctx->plane[2].data;
    uint8_t* ptr = (uint8_t*)buffer;
    size_t const length = ctx->image.width * ctx->image.height;
    if (read)
    {
        for (size_t i = 0u; i < length; ++i)
        {
            SQZ_dwt_coefficient_t const R = *ptr++, G = *ptr++, B = *ptr++;
            *Y++ = (( 33779 * R + 41184 * G + 38182 * B) >> 16) - SQZ_COLOR_LOGL1_LEVEL_OFFSET;
            *c0++ = (-52830 * R +  8188 * G + 37906 * B) >> 16;
            *c1++ = ( 19051 * R - 50317 * G + 37420 * B) >> 16;

        }
    }
    else
    {
        for (size_t i = 0u; i < length; ++i)
        {
            SQZ_dwt_coefficient_t const Y_ = (*Y++) + SQZ_COLOR_LOGL1_LEVEL_OFFSET, c0_ = (*c0++), c1_ = (*c1++);
            SQZ_dwt_coefficient_t const R = (33779 * Y_ - 52830 * c0_ + 19051 * c1_) >> 16;
            SQZ_dwt_coefficient_t const G = (41184 * Y_ +  8188 * c0_ - 50317 * c1_) >> 16;
            SQZ_dwt_coefficient_t const B = (38182 * Y_ + 37906 * c0_ + 37420 * c1_) >> 16;
            *ptr++ = SQZ_COLOR_CLIP(R);
            *ptr++ = SQZ_COLOR_CLIP(G);
            *ptr++ = SQZ_COLOR_CLIP(B);
        }
    }
}

#undef SQZ_COLOR_LOGL1_LEVEL_OFFSET
#undef SQZ_COLOR_CLIP

static void
SQZ_color_process(SQZ_context_t* const ctx, void* const buffer, int const read)
{
    switch (ctx->image.color_mode)
    {
    case SQZ_COLOR_MODE_GRAYSCALE:
    {
        SQZ_color_process_grayscale(ctx, buffer, read);
        break;
    }
    case SQZ_COLOR_MODE_YCOCG_R:
    {
        SQZ_color_process_ycocg_r(ctx, buffer, read);
        break;
    }
    case SQZ_COLOR_MODE_OKLAB:
    {
        SQZ_color_process_oklab(ctx, buffer, read);
        break;
    }
    case SQZ_COLOR_MODE_LOG_L1:
    {
        SQZ_color_process_logl1(ctx, buffer, read);
        break;
    }
    default:
        break;
    }
}

/**
 * \brief           Finds the maximum of the coefficient values in a subband
 * \note            Assumes that all of the subband coefficients have been converted to an explicit
 *                  sign-magnitude representation, forgoing the need to check against absolute values
 * \param[in]       band: The subband to be scanned
 * \return          Maximum coefficient value present in the subband
 */
static SQZ_dwt_coefficient_t
SQZ_dwt_get_max(SQZ_dwt_subband_t* const band)
{
#ifdef DEBUG
    if ((band == NULL) || (band->data == NULL))
    {
        return 0;
    }
#endif
    size_t const width = band->width, height = band->height;
    SQZ_dwt_coefficient_t *ptr, max = *band->data;
    for (size_t y = 0u; y < height; ++y)
    {
        ptr = band->data + y * band->stride;
        for (size_t x = 0u; x < width; ++x)
        {
            if (ptr[x] > max)
            {
                max = ptr[x];
            }
        }
    }
    return max;
}

static void
SQZ_dwt_convert_to_sign_magnitude(SQZ_context_t* const ctx)
{
#ifdef DEBUG
    if (ctx == NULL)
    {
        return;
    }
#endif
    SQZ_dwt_coefficient_t* const data = ctx->data;
    size_t const size = ctx->image.width * ctx->image.height * ctx->image.num_planes;
    for (size_t i = 0u; i < size; ++i)
    {
        data[i] = (data[i] < 0) ? (-2 * data[i]) | 1 : 2 * data[i];
    }
}

static void
SQZ_dwt_convert_from_sign_magnitude(SQZ_context_t* const ctx)
{
#ifdef DEBUG
    if (ctx == NULL)
    {
        return;
    }
#endif
    SQZ_dwt_coefficient_t* const data = ctx->data;
    size_t const size = ctx->image.width * ctx->image.height * ctx->image.num_planes;
    for (size_t i = 0u; i < size; ++i)
    {
        data[i] = (data[i] & 1) ? - (data[i] >> 1) : data[i] >> 1;
    }
}

/*
DWT and iDWT routines are based on the Snow video codec (FFmpeg), by Michael Niedermayer
*/

static void
SQZ_dwt_5_3i_horizontal_pass(SQZ_dwt_coefficient_t* restrict data, SQZ_dwt_coefficient_t* restrict scratch, size_t const width)
{
    if (width < (SQZ_MIN_DIMENSION >> 1u))
    {
        return;
    }
    SQZ_dwt_coefficient_t * restrict odds, * restrict evens = scratch, * restrict l_band = data, * restrict h_band;
    int32_t cf0, cf1, cf2, cf3;
    size_t half_w = width >> 1u, stride = half_w, w = half_w - 1u, i;
    int const odd_w = !!(width & 1u);
    if (odd_w)
    {
        ++stride;
    }
    odds = scratch + stride;
    h_band = data + stride;
    for (i = 0u; i < half_w; ++i)
    {
        evens[i] = data[2u * i];
        odds[i] = data[2u * i + 1u];
    }
    if (odd_w)
        evens[half_w] = data[2u * half_w];
    cf0 = evens[0];
    cf2 = evens[1];
    h_band[0] = cf1 = odds[0] + ((-(cf0 + cf2)) >> 1);
    l_band[0] = cf0 += ((cf1 + 1) >> 1);
    for (i = 1u; i < w; ++i)
    {
        cf3 = odds[i];
        cf0 = evens[i + 1u];
        h_band[i] = cf3 += ((-(cf2 + cf0)) >> 1);
        l_band[i] = cf2 += (cf1 + cf3 + 2) >> 2;
        ++i;
        cf1 = odds[i];
        cf2 = evens[i + 1u];
        h_band[i] = cf1 += ((-(cf2 + cf0)) >> 1);
        l_band[i] = cf0 += (cf1 + cf3 + 2) >> 2;
    }
    h_band[w] = cf3 = odds[w] + (odd_w ? ((-(evens[w] + evens[w + 1u])) >> 1) : -evens[w]);
    l_band[w] = evens[w] + ((h_band[w - 1u] + cf3 + 2) >> 2);
    if (odd_w)
    {
        l_band[w + 1u] = evens[w + 1u] + ((cf3 + 1) >> 1);
    }
}

static void
SQZ_dwt_5_3i(SQZ_dwt_coefficient_t* restrict data, SQZ_dwt_coefficient_t* restrict scratch, size_t const width, size_t const height, size_t const stride)
{
    SQZ_dwt_coefficient_t *nnn = data + SQZ_mirror(-3, height - 1) * stride,
                           *nn  = data + SQZ_mirror(-2, height - 1) * stride;
    for (int32_t i = -2; i < (int32_t)height; i += 2)
    {
        SQZ_dwt_coefficient_t *n = data + SQZ_mirror(i + 1, height - 1) * stride,
                               *r = data + SQZ_mirror(i + 2, height - 1) * stride;
        if (nn <= r)
        {
            SQZ_dwt_5_3i_horizontal_pass(n, scratch, width);
        }
        if ((i + 2) < (int32_t)height)
        {
            SQZ_dwt_5_3i_horizontal_pass(r, scratch, width);
        }
        if (nn <= r)
        {
            for (size_t k = 0u; k < width; ++k)
            {
                n[k] -= (((int32_t)nn[k]) + ((int32_t)r[k])) >> 1;
            }
        }
        if (nnn <= n)
        {
            for (size_t k = 0u; k < width; ++k)
            {
                nn[k] += (((int32_t)nnn[k]) + ((int32_t)n[k]) + 2) >> 2;
            }
        }
        nnn = n;
        nn = r;
    }
}

static SQZ_status_t
SQZ_dwt(SQZ_context_t const * const ctx)
{
#ifdef DEBUG
    if (ctx == NULL)
    {
        return SQZ_INVALID_PARAMETER;
    }
#endif
    size_t const stride = ctx->image.width;
    SQZ_dwt_coefficient_t* scratch = (SQZ_dwt_coefficient_t*)malloc(stride * sizeof(SQZ_dwt_coefficient_t));
    if (scratch == NULL)
    {
        return SQZ_OUT_OF_MEMORY;
    }
    for (size_t plane = 0u; plane < ctx->image.num_planes; ++plane)
    {
        size_t width = stride, height = ctx->image.height;
        for (size_t level = 0u; level < ctx->image.dwt_levels; ++level)
        {
            SQZ_dwt_5_3i(ctx->plane[plane].data, scratch, width, height, stride << level);
            width = (width + 1u) >> 1u;
            height = (height + 1u) >> 1u;
        }
    }
    free(scratch);
    return SQZ_RESULT_OK;
}

static void
SQZ_idwt_5_3i_horizontal_pass(SQZ_dwt_coefficient_t* restrict data, SQZ_dwt_coefficient_t* restrict scratch, size_t const width)
{
    if (width < (SQZ_MIN_DIMENSION >> 1u))
    {
        return;
    }
    SQZ_dwt_coefficient_t * restrict odds, * restrict evens = scratch, * restrict l_band = data, * restrict h_band;
    int32_t cf0, cf1, cf2, cf3;
    size_t half_w = width >> 1u, stride = half_w, w = half_w - 1u, i;
    int const odd_w = !!(width & 1u);
    if (odd_w)
    {
        ++stride;
    }
    odds = scratch + stride;
    h_band = data + stride;
    cf1 = h_band[0];
    evens[0] = cf0 = l_band[0] - ((cf1 + 1) >> 1);
    for (i = 1u; i < w; ++i)
    {
        cf2 = l_band[i];
        cf3 = h_band[i];
        evens[i] = cf2 -= ((cf1 + cf3 + 2) >> 2);
        odds[i - 1] = cf1 - (-(cf0 + cf2) >> 1);
        ++i;
        cf0 = l_band[i];
        cf1 = h_band[i];
        evens[i] = cf0 -= ((cf1 + cf3 + 2) >> 2);
        odds[i - 1] = cf3 - (-(cf0 + cf2) >> 1);
    }
    evens[w] = l_band[w] - ((h_band[w - 1u] + h_band[w] + 2) >> 2);
    odds[w - 1] = h_band[w - 1] - ((-(evens[w - 1] + evens[w])) >> 1);
    if (odd_w)
    {
        evens[w + 1] = l_band[w + 1] - ((h_band[w] + 1) >> 1);
    }
    odds[w] = h_band[w] - (odd_w ? ((-(evens[w] + evens[w + 1u])) >> 1) : -evens[w]);
    for (i = 0u; i < half_w; ++i)
    {
        data[2u * i] = evens[i];
        data[2u * i + 1u] = odds[i];
    }
    if (odd_w)
        data[2u * half_w] = evens[half_w];
}

static void
SQZ_idwt_5_3i(SQZ_dwt_coefficient_t* restrict data, SQZ_dwt_coefficient_t* restrict scratch, size_t const width, size_t const height, size_t const stride)
{
    SQZ_dwt_coefficient_t *nn = data + SQZ_mirror(-2, height - 1) * stride,
                           *n  = data + SQZ_mirror(-1, height - 1) * stride;
    for (int32_t i = -1; i <= (int32_t)height; i += 2)
    {
        SQZ_dwt_coefficient_t *r = data + SQZ_mirror(i + 1, height - 1) * stride,
                               *s = data + SQZ_mirror(i + 2, height - 1) * stride;
        if (n <= s)
        {
            for (size_t k = 0u; k < width; ++k)
            {
                r[k] -= (((int32_t)n[k]) + ((int32_t)s[k]) + 2) >> 2;
            }
        }
        if (nn <= r)
        {
            for (size_t k = 0u; k < width; ++k)
            {
                n[k] += (((int32_t)nn[k]) + ((int32_t)r[k])) >> 1;
            }
        }
        if (i - 1 >= 0)
        {
            SQZ_idwt_5_3i_horizontal_pass(nn, scratch, width);
        }
        if (nn <= r)
        {
            SQZ_idwt_5_3i_horizontal_pass(n, scratch, width);
        }
        nn = r;
        n = s;
    }
}

static SQZ_status_t
SQZ_idwt(SQZ_context_t const* const ctx)
{
#ifdef DEBUG
    if (ctx == NULL)
    {
        return SQZ_INVALID_PARAMETER;
    }
#endif
    size_t const stride = ctx->image.width;
    SQZ_dwt_coefficient_t* scratch = (SQZ_dwt_coefficient_t*)malloc(stride * sizeof(SQZ_dwt_coefficient_t));
    if (scratch == NULL)
    {
        return SQZ_OUT_OF_MEMORY;
    }
    for (size_t plane = 0u; plane < ctx->image.num_planes; ++plane)
    {
        for (int32_t level = (int32_t)ctx->image.dwt_levels - 1; level >= 0; --level)
        {
            size_t width = ctx->image.width, height = ctx->image.height;
            for (int32_t l = level; l > 0; --l)
            {
                width = (width + 1u) >> 1u;
                height = (height + 1u) >> 1u;
            }
            SQZ_idwt_5_3i(ctx->plane[plane].data, scratch, width, height, stride << level);
        }
    }
    free(scratch);
    return SQZ_RESULT_OK;
}

static SQZ_status_t
SQZ_common_init_context(SQZ_context_t* const ctx)
{
#ifdef DEBUG
    if (ctx == NULL)
    {
        return SQZ_INVALID_PARAMETER;
    }
#endif
    ctx->data = (SQZ_dwt_coefficient_t*)calloc(ctx->image.width * ctx->image.height * ctx->image.num_planes, sizeof(SQZ_dwt_coefficient_t));
    if (ctx->data == NULL)
    {
        return SQZ_OUT_OF_MEMORY;
    }
    for (size_t plane = 0u; plane < ctx->image.num_planes; ++plane)
    {
        size_t w = ctx->image.width, h = ctx->image.height;
        ctx->plane[plane].data = ctx->data + plane * w * h;
        for (int32_t level = (int32_t)ctx->image.dwt_levels - 1; level >= 0; --level)
        {
            for (size_t orientation = !!(level > 0); orientation < SQZ_DWT_SUBBANDS; ++orientation)
            {
                SQZ_dwt_subband_t* const band = &ctx->plane[plane].band[level][orientation];
                band->data = ctx->plane[plane].data;
                band->width  = (w + !(orientation & 1u)) >> 1u; /* width of the horizontal lowpass subbands is rounded up */
                band->height = (h + !(orientation > 1u)) >> 1u; /* height of the vertical lowpass subbands is rounded up*/
                band->round = (int)SQZ_schedule[ctx->image.color_mode][plane][level][orientation] + (ctx->image.subsampling & (plane > 0u));
                band->stride = ctx->image.width << (ctx->image.dwt_levels - level);
                if (orientation & 1u)
                {
                    band->data += (w + 1u) >> 1u;
                }
                if (orientation > 1u)
                {
                    band->data += band->stride >> 1u;
                }
            }
            w = (w + 1u) >> 1u;
            h = (h + 1u) >> 1u;
        }
    }
    return SQZ_RESULT_OK;
}

static void
SQZ_common_free_context(SQZ_context_t* const ctx)
{
#ifdef DEBUG
    if (ctx == NULL)
    {
        return;
    }
#endif
    free(ctx->data);
    for (size_t plane = 0u; plane < ctx->image.num_planes; ++plane)
    {
        for (size_t level = 0u; level < ctx->image.dwt_levels; ++level)
        {
            for (size_t orientation = !!(level > 0); orientation < SQZ_DWT_SUBBANDS; ++orientation)
            {
                SQZ_dwt_subband_t* const band = &ctx->plane[plane].band[level][orientation];
                if (band != NULL)
                {
                    free(band->cache.nodes);
                }
            }
        }
    }
}

static SQZ_status_t
SQZ_common_init_subband(SQZ_dwt_subband_t* const band, SQZ_scan_context_t* const scan_ctx)
{
#ifdef DEBUG
    if ((band == NULL) || (scan_ctx == NULL))
    {
        return SQZ_INVALID_PARAMETER;
    }
#endif
    SQZ_status_t result = SQZ_node_cache_init(&band->cache, band->width * band->height);
    if (result != SQZ_RESULT_OK)
    {
        return result;
    }
    SQZ_scan_fn const scan = scan_ctx->scan;
    SQZ_list_init(&band->LIP, &band->cache);
    SQZ_list_init(&band->LSP, &band->cache);
    SQZ_list_init(&band->NSP, &band->cache);
    do
    {
        SQZ_list_add(&band->LIP, (uint16_t)scan_ctx->x, (uint16_t)scan_ctx->y);
    }
    while (scan(scan_ctx));
    return SQZ_RESULT_OK;
}

static SQZ_status_t
SQZ_encode_init_subband(SQZ_dwt_subband_t* const band, SQZ_scan_context_t* const scan_ctx, SQZ_bit_buffer_t* const buffer)
{
#ifdef DEBUG
    if ((band == NULL) || (scan_ctx == NULL) || (buffer == NULL))
    {
        return SQZ_INVALID_PARAMETER;
    }
#endif
    SQZ_status_t result = SQZ_common_init_subband(band, scan_ctx);
    if (result != SQZ_RESULT_OK)
    {
        return result;
    }
    band->max_bitplane = SQZ_ilog2(SQZ_dwt_get_max(band) >> 1);
    band->bitplane = band->max_bitplane;
    SQZ_bit_buffer_write_bits(buffer, band->max_bitplane, 4u);
    return SQZ_RESULT_OK;
}

static SQZ_status_t
SQZ_decode_init_subband(SQZ_dwt_subband_t* const band, SQZ_scan_context_t* const scan_ctx, SQZ_bit_buffer_t* const buffer)
{
#ifdef DEBUG
    if ((band == NULL) || (scan_ctx == NULL) || (buffer == NULL))
    {
        return SQZ_INVALID_PARAMETER;
    }
#endif
    SQZ_status_t result = SQZ_common_init_subband(band, scan_ctx);
    if (result != SQZ_RESULT_OK)
    {
        return result;
    }
    band->max_bitplane = SQZ_bit_buffer_read_bits(buffer, 4u);
    band->bitplane = band->max_bitplane;
    return SQZ_RESULT_OK;
}

static int
SQZ_encode_header(SQZ_image_descriptor_t const * const descriptor, SQZ_bit_buffer_t* const buffer)
{
#ifdef DEBUG
    if ((descriptor == NULL) || (buffer == NULL))
    {
        return 0;
    }
#endif
    SQZ_bit_buffer_write_bits(buffer, SQZ_HEADER_MAGIC,            8u);
    SQZ_bit_buffer_write_bits(buffer, descriptor->width -  1u,    16u);
    SQZ_bit_buffer_write_bits(buffer, descriptor->height - 1u,    16u);
    SQZ_bit_buffer_write_bits(buffer, descriptor->color_mode,      2u);
    SQZ_bit_buffer_write_bits(buffer, descriptor->dwt_levels - 1u, 3u);
    SQZ_bit_buffer_write_bits(buffer, descriptor->scan_order,      2u);
    SQZ_bit_buffer_write_bit(buffer, !!descriptor->subsampling);
    return !SQZ_bit_buffer_eob(buffer);
}

static int
SQZ_decode_header(SQZ_image_descriptor_t* const descriptor, SQZ_bit_buffer_t* const buffer)
{
#ifdef DEBUG
    if ((descriptor == NULL) || (buffer == NULL))
    {
        return 0;
    }
#endif
    if (SQZ_bit_buffer_read_bits(buffer, 8u) != SQZ_HEADER_MAGIC)
    {
        return 0;
    }
    descriptor->width      = SQZ_bit_buffer_read_bits(buffer, 16u) + 1u;
    descriptor->height     = SQZ_bit_buffer_read_bits(buffer, 16u) + 1u;
    descriptor->color_mode = (SQZ_color_mode_t)SQZ_bit_buffer_read_bits(buffer,  2u);
    descriptor->dwt_levels = SQZ_bit_buffer_read_bits(buffer,  3u) + 1u;
    descriptor->scan_order = (SQZ_scan_order_t)SQZ_bit_buffer_read_bits(buffer,  2u);
    descriptor->num_planes = SQZ_number_of_planes[descriptor->color_mode];
    descriptor->subsampling = !!SQZ_bit_buffer_read_bit(buffer);
    return !SQZ_bit_buffer_eob(buffer);
}

static int
SQZ_encode_write_wdr_run(SQZ_bit_buffer_t* const buffer, uint32_t const run)
{
#ifdef DEBUG
    if (buffer == NULL)
    {
        return 0;
    }
#endif
    uint32_t cost = SQZ_ilog2(run) - 1u;
    if (cost <= 16u)
    {
        return SQZ_bit_buffer_write_bits(buffer, SQZ_interleave_u16_to_u32(run), cost * 2u);
    }
    else
    {
        return SQZ_bit_buffer_write_bits(buffer, SQZ_interleave_u16_to_u32(run >> 16), (cost - 16) * 2u) && SQZ_bit_buffer_write_bits(buffer, SQZ_interleave_u16_to_u32(run), 32u);
    }
}

static int
SQZ_decode_read_wdr_run(SQZ_bit_buffer_t* const buffer, uint32_t* const run)
{
#ifdef DEBUG
    if ((buffer == NULL) || (run == NULL))
    {
        return 0;
    }
#endif
    *run = 1u;
    while (SQZ_bit_buffer_read_bit(buffer) == 0)
    {
        int const bit = SQZ_bit_buffer_read_bit(buffer);
        if (bit < 0)
        {
            return 0;
        }
        *run += *run + bit;
    }
    return 1;
}

static int
SQZ_encode_sorting_pass(SQZ_dwt_subband_t* const band, SQZ_bit_buffer_t* const buffer)
{
#ifdef DEBUG
    if ((band == NULL) || (buffer == NULL))
    {
        return 0;
    }
#endif
    SQZ_list_t * restrict const LIP = &band->LIP;
    if ((LIP->length == 0u) || (band->bitplane <= 0))
    {
        return 1;
    }
    SQZ_list_t * restrict const NSP = &band->NSP;
    SQZ_list_node_t *pixel = LIP->head, *previous = NULL;
    SQZ_list_node_t* const base = LIP->cache->nodes;
    SQZ_dwt_coefficient_t const * const data = band->data;
    SQZ_dwt_coefficient_t const bitplane_mask = 1u << band->bitplane;
    size_t const stride = band->stride;
    uint32_t i = 1u, last = 0u;
    while (pixel != NULL)
    {
        SQZ_dwt_coefficient_t const v = data[pixel->y * stride + pixel->x];
        if (!!(v & bitplane_mask))
        {
            if ((!SQZ_bit_buffer_write_bits(buffer, 2u | (v & 1), 1u + !!last)) || (!SQZ_encode_write_wdr_run(buffer, i - last)))
            {
                break;
            }
            last = i;
            pixel = SQZ_list_exchange(LIP, NSP, pixel, previous);
        }
        else
        {
            previous = pixel;
            pixel = SQZ_list_node_next(pixel, base);
        }
        ++i;
    }
    /* now handle WDR termination */
    SQZ_bit_buffer_write_bits(buffer, 3u, 1u + (NSP->length > 0u));
    SQZ_encode_write_wdr_run(buffer, i - last);
    SQZ_bit_buffer_write_bit(buffer, 1u);
    return !SQZ_bit_buffer_eob(buffer);
}

static int
SQZ_decode_sorting_pass(SQZ_dwt_subband_t* const band, SQZ_bit_buffer_t* const buffer)
{
#ifdef DEBUG
    if ((band == NULL) || (buffer == NULL))
    {
        return 0;
    }
#endif
    SQZ_list_t * restrict const LIP = &band->LIP;
    if ((LIP->length == 0u) || (band->bitplane <= 0))
    {
        return 1;
    }
    SQZ_list_t * restrict const NSP = &band->NSP;
    SQZ_list_node_t *pixel = LIP->head, *previous = NULL;
    SQZ_list_node_t* const base = LIP->cache->nodes;
    SQZ_dwt_coefficient_t* const data = band->data;
    SQZ_dwt_coefficient_t const bitplane_mask = 1u << band->bitplane;
    size_t const stride = band->stride;
    uint32_t run;
    int sign;
    do
    {
        sign = SQZ_bit_buffer_read_bit(buffer);
        if ((sign < 0) || (!SQZ_decode_read_wdr_run(buffer, &run)))
        {
            break;
        }
        while ((--run > 0) && (pixel != NULL))
        {
            previous = pixel;
            pixel = SQZ_list_node_next(pixel, base);
        }
        if (pixel != NULL)
        {
            data[pixel->y * stride + pixel->x] |= (bitplane_mask | sign);
            pixel = SQZ_list_exchange(LIP, NSP, pixel, previous);
        }
        else
        {
            break;
        }
    }
    while (1);
    return !SQZ_bit_buffer_eob(buffer);
}

static int
SQZ_encode_refinement_pass(SQZ_dwt_subband_t* const band, SQZ_bit_buffer_t* const buffer)
{
#ifdef DEBUG
    if ((band == NULL) || (buffer == NULL))
    {
        return 0;
    }
#endif
    SQZ_list_node_t* pixel = band->LSP.head;
    SQZ_list_node_t* const base = band->cache.nodes;
    SQZ_dwt_coefficient_t const * const data = band->data;
    SQZ_dwt_coefficient_t const bitplane_mask = 1u << band->bitplane;
    size_t const stride = band->stride;
    while (pixel != NULL)
    {
        SQZ_dwt_coefficient_t const v = data[pixel->y * stride + pixel->x];
        if (!SQZ_bit_buffer_write_bit(buffer, !!(v & bitplane_mask)))
        {
            break;
        }
        pixel = SQZ_list_node_next(pixel, base);
    }
    return !SQZ_bit_buffer_eob(buffer);
}

static int
SQZ_decode_refinement_pass(SQZ_dwt_subband_t* const band, SQZ_bit_buffer_t* const buffer)
{
#ifdef DEBUG
    if ((band == NULL) || (buffer == NULL))
    {
        return 0;
    }
#endif
    SQZ_list_node_t* pixel = band->LSP.head;
    SQZ_list_node_t* const base = band->cache.nodes;
    SQZ_dwt_coefficient_t* const data = band->data;
    SQZ_dwt_coefficient_t const bitplane_mask = 1u << band->bitplane;
    size_t const stride = band->stride;
    while (pixel != NULL)
    {
        int v = SQZ_bit_buffer_read_bit(buffer);
        if (v > 0)
        {
            data[pixel->y * stride + pixel->x] |= bitplane_mask;
        }
        else if (v < 0)
        {
            break;
        }
        pixel = SQZ_list_node_next(pixel, base);
    }
    return !SQZ_bit_buffer_eob(buffer);
}

static int
SQZ_encode_bitplane(SQZ_dwt_subband_t* const band, SQZ_bit_buffer_t* const buffer)
{
#ifdef DEBUG
    if ((band == NULL) || (buffer == NULL))
    {
        return 0;
    }
#endif
    if ((!SQZ_encode_sorting_pass(band, buffer)) || (!SQZ_encode_refinement_pass(band, buffer)))
    {
        return 0;
    }
    /* now merge NSP into LSP */
    SQZ_list_merge(&band->NSP, &band->LSP);
    band->bitplane -= (band->bitplane > 0);
    return !SQZ_bit_buffer_eob(buffer);
}

static int
SQZ_decode_bitplane(SQZ_dwt_subband_t* const band, SQZ_bit_buffer_t* const buffer)
{
#ifdef DEBUG
    if ((band == NULL) || (buffer == NULL))
    {
        return 0;
    }
#endif
    if ((!SQZ_decode_sorting_pass(band, buffer)) || (!SQZ_decode_refinement_pass(band, buffer)))
    {
        return 0;
    }
    /* now merge NSP into LSP */
    SQZ_list_merge(&band->NSP, &band->LSP);
    band->bitplane -= (band->bitplane > 0);
    return !SQZ_bit_buffer_eob(buffer);
}

static void
SQZ_decode_round_coefficients(SQZ_context_t* const ctx)
{
#ifdef DEBUG
    if (ctx == NULL)
    {
        return;
    }
#endif
    for (size_t plane = 0u; plane < ctx->image.num_planes; ++plane)
    {
        for (size_t level = 0u; level < ctx->image.dwt_levels; ++level)
        {
            for (size_t orientation = !!(level > 0); orientation < SQZ_DWT_SUBBANDS; ++orientation)
            {
                SQZ_dwt_subband_t* const band = &ctx->plane[plane].band[level][orientation];
                if ((band->max_bitplane == 0) || (band->bitplane < 2))
                {
                    continue;
                }
                SQZ_list_node_t* pixel = band->LSP.head;
                SQZ_list_node_t* const base = band->cache.nodes;
                SQZ_dwt_coefficient_t* const data = band->data;
                SQZ_dwt_coefficient_t const round_mask = ((1u << band->bitplane) - 1u) ^ 1u;
                size_t const stride = band->stride;
                while (pixel != NULL)
                {
                    data[pixel->y * stride + pixel->x] |= round_mask;
                    pixel = SQZ_list_node_next(pixel, base);
                }
            }
        }
    }
}

static SQZ_status_t
SQZ_schedule_task(SQZ_context_t* const ctx, SQZ_init_subband_fn const init, SQZ_bitplane_task_fn const task)
{
#ifdef DEBUG
    if ((ctx == NULL) || (init == NULL) || (task == NULL))
    {
        return SQZ_INVALID_PARAMETER;
    }
#endif
    SQZ_scan_context_t scan = { 0 };
    SQZ_bit_buffer_t* const buffer = &ctx->buffer;
    size_t state = 0u, plane = 0u, level = 0u, orientation = 0u;
    int round = 0, done = 0;
    scan.type = ctx->image.scan_order;
    while ((!done) && (!SQZ_bit_buffer_eob(buffer)))
    {
        done = 1;
        for (;;)
        {
            SQZ_dwt_subband_t* const band = &ctx->plane[plane].band[level][orientation];
            if ((round < band->round) || ((round > band->round) && (band->bitplane == 0)))
            {
                done &= (round > band->round);
            }
            else
            {
                if (band->round == round)
                {
                    SQZ_scan_init(&scan, band);
                    SQZ_status_t result = init(band, &scan, buffer);
                    if (result != SQZ_RESULT_OK)
                    {
                        free(scan.workspace);
                        return result;
                    }
                }
                if (!task(band, buffer))
                {
                    free(scan.workspace);
                    return SQZ_RESULT_OK;
                }
                done &= (band->bitplane == 0);
            }
            if (!state)
            {
                ++orientation;
                if (orientation >= SQZ_DWT_SUBBANDS)
                {
                    ++level;
                    orientation = !!(level < (size_t)ctx->image.dwt_levels);
                    if (orientation == 0u)
                    {
                        level = 0u;
                        state = plane = (ctx->image.num_planes > 1u);
                        if (!state)
                        {
                            break;
                        }
                    }
                }
            }
            else
            {
                ++plane;
                if (plane >= ctx->image.num_planes)
                {
                    plane = 1u;
                    ++orientation;
                    if (orientation >= SQZ_DWT_SUBBANDS)
                    {
                        ++level;
                        orientation = !!(level < (size_t)ctx->image.dwt_levels);
                        if (orientation == 0u)
                        {
                            level = 0u;
                            state = plane = 0u;
                            break;
                        }
                    }
                }
            }
        }
        ++round;
    };
    free(scan.workspace);
    return SQZ_RESULT_OK;
}

#undef SQZ_DWT_SUBBANDS

static SQZ_status_t
SQZ_validate_input(SQZ_image_descriptor_t* const descriptor, int const read_only)
{
#ifdef DEBUG
    if (descriptor == NULL)
    {
        return SQZ_INVALID_PARAMETER;
    }
#endif
    if ((descriptor->width  < SQZ_MIN_DIMENSION) || (descriptor->width  > SQZ_MAX_DIMENSION) ||
            (descriptor->height < SQZ_MIN_DIMENSION) || (descriptor->height > SQZ_MAX_DIMENSION) ||
            (descriptor->color_mode < SQZ_COLOR_MODE_GRAYSCALE) || (descriptor->color_mode >= SQZ_COLOR_MODE_COUNT) ||
            (descriptor->scan_order < SQZ_SCAN_ORDER_RASTER) || (descriptor->scan_order >= SQZ_SCAN_ORDER_COUNT) ||
            (descriptor->dwt_levels == 0u) || (descriptor->dwt_levels > SQZ_DWT_MAX_LEVEL)
       )
    {
        return (read_only) ? SQZ_DATA_CORRUPTED : SQZ_INVALID_PARAMETER;
    }
    size_t const smallest_dimension = (descriptor->width > descriptor->height) ? descriptor->height : descriptor->width;
    uint32_t max_level = SQZ_ilog2(smallest_dimension) - 3u;
    if (max_level > SQZ_DWT_MAX_LEVEL)
    {
        max_level = SQZ_DWT_MAX_LEVEL;
    }
    if (descriptor->dwt_levels > max_level)
    {
        if (read_only)
        {
            return SQZ_DATA_CORRUPTED;
        }
        else
        {
            descriptor->dwt_levels = max_level;
        }
    }
    if (!read_only)
    {
        descriptor->num_planes = SQZ_number_of_planes[descriptor->color_mode];
    }
    return SQZ_RESULT_OK;
}

SQZ_status_t
SQZ_encode(void* const source, void* const dest, SQZ_image_descriptor_t* const descriptor, size_t* const budget)
{
    SQZ_status_t result = SQZ_validate_input(descriptor, 0);
    if (result != SQZ_RESULT_OK)
    {
        return result;
    }
    SQZ_context_t ctx = { 0 };
    memcpy(&ctx.image, descriptor, sizeof(*descriptor));
    SQZ_bit_buffer_init(&ctx.buffer, dest, *budget);
    if (!SQZ_encode_header(descriptor, &ctx.buffer))
    {
        return SQZ_BUFFER_TOO_SMALL;
    }
    result = SQZ_common_init_context(&ctx);
    if (result != SQZ_RESULT_OK)
    {
        SQZ_common_free_context(&ctx);
        return result;
    }
    SQZ_color_process(&ctx, source, 1);
    result = SQZ_dwt(&ctx);
    if (result != SQZ_RESULT_OK)
    {
        SQZ_common_free_context(&ctx);
        return result;
    }
    SQZ_dwt_convert_to_sign_magnitude(&ctx);
    result = SQZ_schedule_task(&ctx, &SQZ_encode_init_subband, &SQZ_encode_bitplane);
    if (result != SQZ_RESULT_OK)
    {
        SQZ_common_free_context(&ctx);
        return result;
    }
    *budget = (SQZ_bit_buffer_bits_used(&ctx.buffer) + (CHAR_BIT - 1)) / CHAR_BIT;
    SQZ_common_free_context(&ctx);
    return SQZ_RESULT_OK;
}

SQZ_status_t
SQZ_decode(void* const source, void* const dest, size_t const src_size, size_t* const dest_size, SQZ_image_descriptor_t* const descriptor)
{
    if ((source == NULL) || (dest_size == NULL) || ((dest == NULL) && (*dest_size != 0u)))
    {
        return SQZ_INVALID_PARAMETER;
    }
    SQZ_context_t ctx = { 0 };
    SQZ_bit_buffer_init(&ctx.buffer, source, src_size);
    if (!SQZ_decode_header(&ctx.image, &ctx.buffer))
    {
        return SQZ_INVALID_PARAMETER;
    }
    SQZ_status_t result = SQZ_validate_input(&ctx.image, 1);
    if (result != SQZ_RESULT_OK)
    {
        return result;
    }
    if (descriptor != NULL)
    {
        memcpy(descriptor, &ctx.image, sizeof(*descriptor));
    }
    size_t const length = ctx.image.width * ctx.image.height * ctx.image.num_planes;
    if (*dest_size < length)
    {
        *dest_size = length;
        return SQZ_BUFFER_TOO_SMALL;
    }
    result = SQZ_common_init_context(&ctx);
    if (result != SQZ_RESULT_OK)
    {
        SQZ_common_free_context(&ctx);
        return result;
    }
    result = SQZ_schedule_task(&ctx, &SQZ_decode_init_subband, &SQZ_decode_bitplane);
    if (result != SQZ_RESULT_OK)
    {
        SQZ_common_free_context(&ctx);
        return result;
    }
    SQZ_decode_round_coefficients(&ctx);
    SQZ_dwt_convert_from_sign_magnitude(&ctx);
    result = SQZ_idwt(&ctx);
    if (result != SQZ_RESULT_OK)
    {
        SQZ_common_free_context(&ctx);
        return result;
    }
    SQZ_color_process(&ctx, dest, 0);
    SQZ_common_free_context(&ctx);
    return SQZ_RESULT_OK;
}

#endif /* SQZ_IMPLEMENTATION */
