/**
 * \file            sqz.cpp
 * \brief           Example usage of the SQZ image compression library
 */

/*
                    Copyright (c) 2024, Márcio Pais

                    SPDX-License-Identifier: MIT

Requirements:
    - "stb_image.h"         [https://github.com/nothings/stb/blob/master/stb_image.h]
    - "stb_image_write.h"   [https://github.com/nothings/stb/blob/master/stb_image_write.h]
    - "CLI11.hpp"           [https://github.com/CLIUtils/CLI11]
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>

#define SQZ_IMPLEMENTATION
#include "sqz.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

void usage(char* progname)
{
    fprintf(stderr,
        "%s %s %s\n",
        "Usage:", progname, "[-h] [-c budget] [-d] [-l level] [-m mode] [-o order] input output\n"
        "SQZ encode/decode an image.\n"
     );
}

void help()
{
    fprintf(stderr,
        "%s\n",
        "-c budget         Requested output image size\n"
        "-d                Decode\n"
        "-l level          Number of DWT decompositions to perform (default: 5)\n"
        "-m mode           Internal color mode (default: Grayscale / YCoCg-R)\n0: Grayscale\n1: YCoCg-R\n2: Oklab\n3: logl1\n"
        "-o order          DWT coefficient scanning order (default: Snake)\n0: Raster\n1: Snake\n2: Morton\n3: Hilbert\n"
        "-s subsampling    Use additional chroma subsampling\n"
        "\n"
        "stb_image and stb_image_write by Sean Barrett and others is used to read and\n"
        "write images.\n"
    );
}

int main(int argc, char** argv)
{
    SQZ_image_descriptor_t image = {};
    size_t budget = 0u;
    FILE *input = NULL, *output = NULL;
    uint8_t *src = NULL, *buffer = NULL;
    bool decode = false;
    int levels = 5, color_mode = 1, scan_order = 1, subsampling = 0;

    int opt;
    while ( (opt = getopt(argc, argv, "c:dl:m:o:s:h")) != -1 )
    {
        switch(opt)
        {
            case 'c':
                budget = atoi(optarg);
                break;
            case 'd':
                decode = true;
                break;
            case 'l':
                levels = atoi(optarg);
                break;
            case 'm':
                color_mode = atoi(optarg);
                break;
            case 'o':
                scan_order = atoi(optarg);
                break;
            case 's':
                subsampling = atoi(optarg);
                break;
            case 'h':
                usage(argv[0]);
                help();
                return 0;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    // Need at least two filenames after the last option
    if (argc < optind + 2)
    {
        usage(argv[0]);
        return 1;
    }

    if (decode)
    {
        input = fopen(argv[optind], "rb");
        if (input == NULL)
        {
            fprintf(stderr, "Error reading input image");
            return 1;
        }
        fseek(input, 0, SEEK_END);
        size_t size = ftell(input);
        fseek(input, 0, SEEK_SET);
        if ((budget < SQZ_HEADER_MAGIC + 1u) || (budget > size))
        {
            budget = size;
        }
        size = 0u;
        src = (uint8_t*)malloc(budget);
        if (src == NULL)
        {
            fprintf(stderr, "Insufficient memory");
            fclose(input);
            return 2;
        }
        if (fread(src, sizeof(uint8_t), budget, input) != budget)
        {
            fprintf(stderr, "Error reading input image");
            fclose(input);
            free(src);
            return 3;
        }
        fclose(input);
        SQZ_status_t result = SQZ_decode(src, buffer, budget, &size, &image);
        if (result != SQZ_BUFFER_TOO_SMALL)
        {
            fprintf(stderr, "Error parsing SQZ image, code: %d", (int)result);
            free(src);
            return (int)result;
        }
        buffer = (uint8_t*)malloc(size);
        if (buffer == NULL)
        {
            fprintf(stderr, "Insufficient memory");
            free(src);
            return 4;
        }
        result = SQZ_decode(src, buffer, budget, &size, &image);
        free(src);
        if (result != SQZ_RESULT_OK)
        {
            fprintf(stderr, "Error decompressing SQZ image, code: %d", (int)result);
        }
        else
        {
			if (!stbi_write_png(argv[optind + 1], (int)image.width, (int)image.height, (int)image.num_planes, buffer, 0))
			{
				fprintf(stderr, "Error writing output PNG image");
				free(buffer);
				return 5;
			}
        }
        free(buffer);
        return (int)result;
	}
    else
    {
        int width = 0, height = 0, channels = 0;
        if (!stbi_info(argv[optind], &width, &height, &channels) || (width <= 0) || (height <= 0) || ((channels != 1) && (channels != 3)))
        {
            fprintf(stderr, "Invalid image header, parsing failed");
            return 1;
        }
        image.width = (size_t)width;
        image.height = (size_t)height;
        image.num_planes = (size_t)channels;
        image.dwt_levels = levels;
        image.color_mode = color_mode;
        image.scan_order = scan_order;
        image.subsampling = subsampling;
        if ((channels == 1) && (image.color_mode > SQZ_COLOR_MODE_GRAYSCALE))
        {
            image.color_mode = SQZ_COLOR_MODE_GRAYSCALE;
        }
        src = (uint8_t*)stbi_load(argv[optind], &width, &height, 0, channels);
        if (src == NULL)
        {
            fprintf(stderr, "Error loading input image");
            return 2;
        }
        if (budget < SQZ_HEADER_SIZE + 1u)      /* assume (near) lossless compression expected */
        {
            budget = image.width * image.height * image.num_planes;
            budget += budget >> 2u;
        }
        buffer = (uint8_t*)calloc(budget, sizeof(uint8_t));
        if (buffer == NULL)
        {
            fprintf(stderr, "Insufficient memory");
            return 7;
        }
        SQZ_status_t result = SQZ_encode(src, buffer, &image, &budget);
        free(src);
        if (result != SQZ_RESULT_OK)
        {
            fprintf(stderr, "Error compressing image, code: %d", (int)result);
        }
        else
        {
            output = fopen(argv[optind + 1], "wb");
            if (output == NULL)
            {
                fprintf(stderr, "Error creating output image");
                return 8;
            }
            fwrite(buffer, sizeof(uint8_t), budget, output);
            fclose(output);
        }
        free(buffer);
        return (int)result;
    }
}
