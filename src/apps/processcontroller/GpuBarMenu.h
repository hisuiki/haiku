/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _GPU_BAR_MENU_H_
#define _GPU_BAR_MENU_H_

#include "Utilities.h"
#include <Menu.h>

class GpuStatusBarMenuItem;
class GpuBarMenuItem;

class GpuBarMenu : public BMenu {
public:
					GpuBarMenu(const char* title, info_pack* infos, int32 teamCount);
	virtual			~GpuBarMenu();

	virtual	void	Draw(BRect updateRect);
	virtual	void	Pulse();

private:
	void			_Refresh();

	GpuStatusBarMenuItem* fStatusItem;
	info_pack*		fInfos;
	int32			fTeamCount;
	bool			fFirstShow;
};

#endif // _GPU_BAR_MENU_H_
