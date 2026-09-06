/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include "GpuBarMenuItem.h"

#include "Colors.h"
#include "ProcessController.h"
#include "Utilities.h"

#include <Bitmap.h>
#include <ControlLook.h>
#include <stdio.h>
#include <math.h>

static const float kStatusTextWidth = 110.0f;

GpuBarMenuItem::GpuBarMenuItem(const char* name, team_id team,
	uint32 contexts, double usage, uint64 ticks,
	BBitmap* icon, bool deleteIcon)
	:
	IconMenuItem(icon, name, NULL, true, deleteIcon),
	fTeamID(team),
	fContexts(contexts),
	fUsage(usage),
	fTicks(ticks),
	fLastFillWidth(-1.0f),
	fLastContexts(0xFFFFFFFF),
	fLastUsage(-1.0)
{
}

GpuBarMenuItem::~GpuBarMenuItem()
{
}

void
GpuBarMenuItem::Update(uint32 contexts, double usage, uint64 ticks)
{
	fContexts = contexts;
	fUsage = usage;
	fTicks = ticks;
}

void
GpuBarMenuItem::GetContentSize(float* width, float* height)
{
	IconMenuItem::GetContentSize(width, height);
	if (width != NULL)
		*width += kBarWidth + kStatusTextWidth + 20;
}

void
GpuBarMenuItem::DrawContent()
{
	DrawIcon();

	BPoint loc = ContentLocation();
	loc.x += ceilf(be_control_look->DefaultLabelSpacing() * 3.3f);
	Menu()->MovePenTo(loc);
	BMenuItem::DrawContent();

	DrawBar(true);
}

void
GpuBarMenuItem::DrawBar(bool force)
{
	const bool selected = IsSelected();
	BRect frame = Frame();
	BMenu* menu = Menu();
	rgb_color highColor = menu->HighColor();

	BFont font;
	menu->GetFont(&font);
	BRect bar = bar_rect(frame, &font);

	bar.right -= kStatusTextWidth;
	bar.left = bar.right - kBarWidth;

	if (bar.left < frame.left + 60)
		return;

	if (force) {
		menu->SetHighColor(selected ? gFrameColorSelected : gFrameColor);
		menu->StrokeRect(bar);
	}

	bar.InsetBy(1, 1);
	float totalWidth = bar.Width();
	float fillWidth = floorf(totalWidth * (float)fUsage);
	if (fillWidth > totalWidth) fillWidth = totalWidth;
	if (fillWidth < 0) fillWidth = 0;

	if (!force && fLastFillWidth == fillWidth && fLastContexts == fContexts
		&& fabs(fLastUsage - fUsage) < 0.005) {
		return;
	}

	BRect fillRect = bar;
	fillRect.right = bar.left + fillWidth;

	if (fillRect.Width() > 0) {
		menu->SetHighColor(selected ? gGpuColorSelected : gGpuColor);
		menu->FillRect(fillRect);
	}

	BRect emptyRect = bar;
	emptyRect.left = fillRect.right + 1;
	if (emptyRect.left <= emptyRect.right) {
		menu->SetHighColor(selected ? gWhiteSelected : kWhite);
		menu->FillRect(emptyRect);
	}

	fLastFillWidth = fillWidth;
	fLastUsage = fUsage;

	// Status text (e.g. "98% (2 ctx)")
	BRect textRect(bar.right + 1, frame.top, frame.right, frame.bottom);
	menu->SetLowColor(selected ? gMenuBackColorSelected : gMenuBackColor);
	menu->FillRect(textRect, B_SOLID_LOW);

	char statusText[64];
	if (fContexts > 0) {
		snprintf(statusText, sizeof(statusText), "%3.0f%% (%u ctx)",
			fUsage * 100.0, fContexts);
	} else {
		snprintf(statusText, sizeof(statusText), "%3.0f%%",
			fUsage * 100.0);
	}

	font_height fh;
	font.GetHeight(&fh);
	BPoint textPoint;
	textPoint.x = bar.right + 8;
	textPoint.y = frame.top + (frame.Height() + fh.ascent - fh.descent) / 2.0f;

	menu->SetHighColor(selected ? ui_color(B_MENU_SELECTED_ITEM_TEXT_COLOR)
		: ui_color(B_MENU_ITEM_TEXT_COLOR));
	menu->DrawString(statusText, textPoint);

	fLastContexts = fContexts;
	menu->SetHighColor(highColor);
}

// --------------------------------------------------------------------------
// GpuStatusBarMenuItem

GpuStatusBarMenuItem::GpuStatusBarMenuItem(const char* deviceName, uint32 freqMhz, double load)
	:
	BMenuItem("", NULL),
	fDeviceName(deviceName),
	fFrequencyMhz(freqMhz),
	fLoad(load),
	fLastFillWidth(-1.0f),
	fLastFreq(0),
	fLastLoad(-1.0)
{
	SetEnabled(false);
}

GpuStatusBarMenuItem::~GpuStatusBarMenuItem()
{
}

void
GpuStatusBarMenuItem::Update(const char* deviceName, uint32 freqMhz, double load)
{
	if (deviceName != NULL)
		fDeviceName = deviceName;
	fFrequencyMhz = freqMhz;
	fLoad = load;
}

void
GpuStatusBarMenuItem::GetContentSize(float* width, float* height)
{
	BMenuItem::GetContentSize(width, height);
	BFont font;
	Menu()->GetFont(&font);

	char label[128];
	if (fFrequencyMhz > 0)
		snprintf(label, sizeof(label), "%s (%u MHz)", fDeviceName.String(), fFrequencyMhz);
	else
		snprintf(label, sizeof(label), "%s", fDeviceName.String());

	if (width != NULL)
		*width = font.StringWidth(label) + kBarWidth + kStatusTextWidth + 40;
	if (height != NULL)
		*height = font.Size() + 6;
}

void
GpuStatusBarMenuItem::DrawContent()
{
	BRect frame = Frame();
	BMenu* menu = Menu();
	rgb_color highColor = menu->HighColor();

	BFont font;
	menu->GetFont(&font);
	font_height fh;
	font.GetHeight(&fh);

	char label[128];
	if (fFrequencyMhz > 0)
		snprintf(label, sizeof(label), "%s (%u MHz)", fDeviceName.String(), fFrequencyMhz);
	else
		snprintf(label, sizeof(label), "%s", fDeviceName.String());

	BPoint loc = ContentLocation();
	loc.x += ceilf(be_control_look->DefaultLabelSpacing());
	loc.y = frame.top + (frame.Height() + fh.ascent - fh.descent) / 2.0f;

	menu->SetHighColor(ui_color(B_MENU_ITEM_TEXT_COLOR));
	menu->DrawString(label, loc);

	DrawBar(true);
	menu->SetHighColor(highColor);
}

void
GpuStatusBarMenuItem::DrawBar(bool force)
{
	BRect frame = Frame();
	BMenu* menu = Menu();
	rgb_color highColor = menu->HighColor();

	BFont font;
	menu->GetFont(&font);
	font_height fh;
	font.GetHeight(&fh);

	BRect bar = bar_rect(frame, &font);
	bar.right -= kStatusTextWidth;
	bar.left = bar.right - kBarWidth;

	if (bar.left < frame.left + 60)
		return;

	if (force) {
		menu->SetHighColor(gFrameColor);
		menu->StrokeRect(bar);
	}

	bar.InsetBy(1, 1);
	float totalWidth = bar.Width();
	float fillWidth = floorf(totalWidth * (float)fLoad);
	if (fillWidth > totalWidth) fillWidth = totalWidth;
	if (fillWidth < 0) fillWidth = 0;

	if (!force && fLastFillWidth == fillWidth && fLastFreq == fFrequencyMhz
		&& fabs(fLastLoad - fLoad) < 0.005) {
		return;
	}

	BRect fillRect = bar;
	fillRect.right = bar.left + fillWidth;
	if (fillRect.Width() > 0) {
		menu->SetHighColor(gGpuColor);
		menu->FillRect(fillRect);
	}

	BRect emptyRect = bar;
	emptyRect.left = fillRect.right + 1;
	if (emptyRect.left <= emptyRect.right) {
		menu->SetHighColor(kWhite);
		menu->FillRect(emptyRect);
	}

	fLastFillWidth = fillWidth;
	fLastFreq = fFrequencyMhz;
	fLastLoad = fLoad;

	BRect textRect(bar.right + 1, frame.top, frame.right, frame.bottom);
	menu->SetLowColor(gMenuBackColor);
	menu->FillRect(textRect, B_SOLID_LOW);

	char loadText[32];
	snprintf(loadText, sizeof(loadText), "%3.0f%% load", fLoad * 100.0);
	BPoint textPoint(bar.right + 8, frame.top + (frame.Height() + fh.ascent - fh.descent) / 2.0f);
	menu->SetHighColor(ui_color(B_MENU_ITEM_TEXT_COLOR));
	menu->DrawString(loadText, textPoint);

	menu->SetHighColor(highColor);
}
