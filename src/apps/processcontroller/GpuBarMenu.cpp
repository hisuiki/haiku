/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include "GpuBarMenu.h"
#include "GpuBarMenuItem.h"
#include "GpuQuery.h"
#include "ProcessController.h"

#include <MenuItem.h>
#include <SeparatorItem.h>
#include <Window.h>
#include <stdio.h>

GpuBarMenu::GpuBarMenu(const char* title, info_pack* infos, int32 teamCount)
	:
	BMenu(title),
	fInfos(infos),
	fTeamCount(teamCount),
	fFirstShow(true)
{
	SetFlags(Flags() | B_PULSE_NEEDED);
	SetFont(be_plain_font);

	const char* devName = gGpuQuery ? gGpuQuery->GetDeviceName() : "Intel GPU";
	uint32 freq = gGpuQuery ? gGpuQuery->GetFrequencyMhz() : 0;
	double load = gGpuQuery ? gGpuQuery->GetTotalGpuUsage() : 0.0;

	fStatusItem = new GpuStatusBarMenuItem(devName, freq, load);
	AddItem(fStatusItem);
	AddItem(new BSeparatorItem());

	_Refresh();
}

GpuBarMenu::~GpuBarMenu()
{
}

void
GpuBarMenu::Draw(BRect updateRect)
{
	BMenu::Draw(updateRect);
	if (fFirstShow) {
		Pulse();
		fFirstShow = false;
	}
}

void
GpuBarMenu::Pulse()
{
	if (gGpuQuery)
		gGpuQuery->Query();

	if (Window() != NULL) {
		Window()->BeginViewTransaction();
		_Refresh();
		Window()->EndViewTransaction();
		Window()->Flush();
	} else {
		_Refresh();
	}
}

void
GpuBarMenu::_Refresh()
{
	if (!gGpuQuery)
		return;

	const char* devName = gGpuQuery->GetDeviceName();
	uint32 freq = gGpuQuery->GetFrequencyMhz();
	double load = gGpuQuery->GetTotalGpuUsage();
	fStatusItem->Update(devName, freq, load);
	fStatusItem->DrawBar(false);

	uint32 clientCount = gGpuQuery->GetClientCount();

	if (clientCount == 0) {
		// Remove process items
		while (CountItems() > 2) {
			BMenuItem* item = RemoveItem(2);
			delete item;
		}
		if (CountItems() == 2) {
			BMenuItem* empty = new BMenuItem("No active GPU processes", NULL);
			empty->SetEnabled(false);
			AddItem(empty);
		}
		return;
	}

	// Remove placeholder if present
	if (CountItems() == 3) {
		BMenuItem* item = ItemAt(2);
		if (dynamic_cast<GpuBarMenuItem*>(item) == NULL) {
			RemoveItem(2);
			delete item;
		}
	}

	// Track which items matched
	bool matched[16] = { false };

	// Update existing items or mark for removal
	int32 idx = 2;
	while (idx < CountItems()) {
		GpuBarMenuItem* item = dynamic_cast<GpuBarMenuItem*>(ItemAt(idx));
		if (item == NULL) {
			idx++;
			continue;
		}

		bool stillExists = false;
		for (uint32 c = 0; c < clientCount && c < 16; c++) {
			GpuClientInfo client;
			if (gGpuQuery->GetClientInfo(c, client) && client.team == item->Team()) {
				item->Update(client.contexts, client.usage, client.ticks);
				item->DrawBar(false);
				matched[c] = true;
				stillExists = true;
				break;
			}
		}

		if (!stillExists) {
			RemoveItem(idx);
			delete item;
		} else {
			idx++;
		}
	}

	// Add any new clients that weren't in the menu
	for (uint32 c = 0; c < clientCount && c < 16; c++) {
		if (matched[c])
			continue;

		GpuClientInfo client;
		if (!gGpuQuery->GetClientInfo(c, client))
			continue;

		char name[B_PATH_NAME_LENGTH] = "Unknown";
		BBitmap* icon = NULL;

		bool found = false;
		if (fInfos != NULL) {
			for (int32 k = 0; k < fTeamCount; k++) {
				if (fInfos[k].team_info.team == client.team) {
					strlcpy(name, fInfos[k].team_name, sizeof(name));
					icon = fInfos[k].team_icon;
					found = true;
					break;
				}
			}
		}

		if (!found) {
			info_pack pack;
			if (get_team_info(client.team, &pack.team_info) == B_OK) {
				pack.team_icon = NULL;
				get_team_name_and_icon(pack, true);
				strlcpy(name, pack.team_name, sizeof(name));
				icon = pack.team_icon;
			} else {
				snprintf(name, sizeof(name), "Team %" B_PRId32, client.team);
			}
		}

		GpuBarMenuItem* item = new GpuBarMenuItem(name, client.team,
			client.contexts, client.usage, client.ticks, icon, false);
		AddItem(item);
		item->DrawBar(true);
	}
}
