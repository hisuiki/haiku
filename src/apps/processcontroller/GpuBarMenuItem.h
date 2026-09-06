/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _GPU_BAR_MENU_ITEM_H_
#define _GPU_BAR_MENU_ITEM_H_

#include "IconMenuItem.h"
#include <String.h>

class GpuBarMenuItem : public IconMenuItem {
public:
					GpuBarMenuItem(const char* name, team_id team,
						uint32 contexts, double usage, uint64 ticks,
						BBitmap* icon, bool deleteIcon);
	virtual			~GpuBarMenuItem();

	virtual	void	DrawContent();
	virtual	void	GetContentSize(float* width, float* height);

	void			DrawBar(bool force);
	void			Update(uint32 contexts, double usage, uint64 ticks);

	team_id			Team() const { return fTeamID; }
	double			Usage() const { return fUsage; }
	uint32			Contexts() const { return fContexts; }

private:
	team_id			fTeamID;
	uint32			fContexts;
	double			fUsage;
	uint64			fTicks;
	float			fLastFillWidth;
	uint32			fLastContexts;
	double			fLastUsage;
};

class GpuStatusBarMenuItem : public BMenuItem {
public:
					GpuStatusBarMenuItem(const char* deviceName, uint32 freqMhz, double load);
	virtual			~GpuStatusBarMenuItem();

	virtual	void	DrawContent();
	virtual	void	GetContentSize(float* width, float* height);

	void			DrawBar(bool force);
	void			Update(const char* deviceName, uint32 freqMhz, double load);

private:
	BString			fDeviceName;
	uint32			fFrequencyMhz;
	double			fLoad;
	float			fLastFillWidth;
	uint32			fLastFreq;
	double			fLastLoad;
};

#endif // _GPU_BAR_MENU_ITEM_H_
