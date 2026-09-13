/*
 * Copyright 2008-2010, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Artur Wyszynski <harakash@gmail.com>
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <KernelExport.h>

#define __BOOTSPLASH_KERNEL__
#include <boot/images.h>
#include <boot/platform/generic/video_blitter.h>
#include <boot/platform/generic/video_splash.h>

#include <boot_item.h>
#include <debug.h>
#include <frame_buffer_console.h>

#include <boot_splash.h>


//#define TRACE_BOOT_SPLASH 1
#ifdef TRACE_BOOT_SPLASH
#	define TRACE(x...) dprintf(x);
#else
#	define TRACE(x...) ;
#endif


static struct frame_buffer_boot_info *sInfo;
static uint8 *sUncompressedIcons;


//	#pragma mark - exported functions


void
boot_splash_init(uint8 *bootSplash)
{
	TRACE("boot_splash_init: enter\n");

	if (debug_screen_output_enabled())
		return;

	sInfo = (frame_buffer_boot_info *)get_boot_item(FRAME_BUFFER_BOOT_INFO,
		NULL);

	sUncompressedIcons = bootSplash;
}


/*!	Paints the whole boot frame buffer in one colour, so that nothing of the
	splash is left in it.
*/
static void
clear_frame_buffer(uint8 red, uint8 green, uint8 blue)
{
	uint8* row = (uint8*)sInfo->frame_buffer;

	for (int32 y = 0; y < sInfo->height; y++) {
		switch (sInfo->depth) {
			case 32:
			case 24:
			{
				uint8* pixel = row;
				for (int32 x = 0; x < sInfo->width; x++) {
					pixel[0] = blue;
					pixel[1] = green;
					pixel[2] = red;
					if (sInfo->depth == 32)
						pixel[3] = 255;
					pixel += sInfo->depth / 8;
				}
				break;
			}

			case 16:
			{
				uint16 value = ((uint16)(red >> 3) << 11)
					| ((uint16)(green >> 2) << 5) | (blue >> 3);
				uint16* pixel = (uint16*)row;
				for (int32 x = 0; x < sInfo->width; x++)
					pixel[x] = value;
				break;
			}

			case 15:
			{
				uint16 value = ((uint16)(red >> 3) << 10)
					| ((uint16)(green >> 3) << 5) | (blue >> 3);
				uint16* pixel = (uint16*)row;
				for (int32 x = 0; x < sInfo->width; x++)
					pixel[x] = value;
				break;
			}

			default:
				// An indexed mode has no colour to write without the palette;
				// leaving the splash there is better than writing nonsense.
				return;
		}

		row += sInfo->bytes_per_row;
	}
}


void
boot_splash_uninit(void)
{
	// Paint over the splash now that the boot has got this far. The image sits
	// in the frame buffer the boot loader set up, and stays there: anything
	// that points the display back at that buffer later - a desktop taking
	// over the screen at a login, a logout, or a switch between users - would
	// show the boot splash again, long after the boot. What is left behind is
	// the same plain grey a desktop paints while it starts.
	if (sInfo != NULL)
		clear_frame_buffer(96, 96, 96);

	sInfo = NULL;
}


void
boot_splash_set_stage(int stage)
{
	TRACE("boot_splash_set_stage: stage=%d\n", stage);

	if (sInfo == NULL || stage < 0 || stage >= BOOT_SPLASH_STAGE_MAX)
		return;

	int width, height, x, y;
	compute_splash_icons_placement(sInfo->width, sInfo->height,
		width, height, x, y);

	int stageLeftEdge = width * stage / BOOT_SPLASH_STAGE_MAX;
	int stageRightEdge = width * (stage + 1) / BOOT_SPLASH_STAGE_MAX;

	BlitParameters params;
	params.from = sUncompressedIcons;
	params.fromWidth = kSplashIconsWidth;
	params.fromLeft = stageLeftEdge;
	params.fromTop = 0;
	params.fromRight = stageRightEdge;
	params.fromBottom = height;
	params.to = (uint8*)sInfo->frame_buffer;
	params.toBytesPerRow = sInfo->bytes_per_row;
	params.toLeft = stageLeftEdge + x;
	params.toTop = y;

	blit(params, sInfo->depth);
}
