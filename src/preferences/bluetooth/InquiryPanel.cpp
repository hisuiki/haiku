/*
 * Copyright 2008-2009, Oliver Ruiz Dorantes, <oliver.ruiz.dorantes@gmail.com>
 * Copyright 2021, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		Fredrik Modéen <fredrik_at_modeen.se>
 */

#include <Alert.h>
#include <Button.h>
#include <Catalog.h>
#include <DurationFormat.h>
#include <LayoutBuilder.h>
#include <ListItem.h>
#include <ListView.h>
#include <MessageRunner.h>
#include <ScrollView.h>
#include <SpaceLayoutItem.h>
#include <StatusBar.h>
#include <StringFormat.h>
#include <TabView.h>
#include <TextView.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/DiscoveryAgent.h>
#include <bluetooth/DiscoveryListener.h>
#include <bluetooth/LocalDevice.h>

#include "defs.h"
#include "DeviceListItem.h"
#include "InquiryPanel.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Inquiry panel"

using Bluetooth::DeviceListItem;

// private funcionaility provided by kit
extern uint8 GetInquiryTime();

static const uint32 kMsgStart = 'InSt';
static const uint32 kMsgFinish = 'InFn';
static const uint32 kMsgCancel = 'InCl';
static const uint32 kMsgShowDebug = 'ShDG';

static const uint32 kMsgInquiry = 'iQbt';
static const uint32 kMsgAddListDevice = 'aDdv';

static const uint32 kMsgSelected = 'isLt';
static const uint32 kMsgSecond = 'sCMs';


class PanelDiscoveryListener : public DiscoveryListener {

public:

	PanelDiscoveryListener(InquiryPanel* iPanel)
		:
		DiscoveryListener(),
		fInquiryPanel(iPanel)
	{

	}


	void
	DeviceDiscovered(RemoteDevice* btDevice, DeviceClass cod)
	{
		BMessage* message = new BMessage(kMsgAddListDevice);
		message->AddPointer("remoteItem", new DeviceListItem(btDevice));
		fInquiryPanel->PostMessage(message);
	}


	void
	InquiryResponse(int discType)
	{
		BMessage* message;
		switch (discType) {
			case BT_INQUIRY_COMPLETED:
				message = new BMessage(kMsgFinish);
				fInquiryPanel->PostMessage(message);
				break;

			case BT_INQUIRY_TERMINATED:
				message = new BMessage(kMsgCancel);
				fInquiryPanel->PostMessage(message);
				break;

			case BT_INQUIRY_ERROR:
				break;
		}
	}


	void
	InquiryStarted(status_t status)
	{
		BMessage* message = new BMessage(kMsgStart);
		fInquiryPanel->PostMessage(message);
	}

private:
	InquiryPanel*	fInquiryPanel;

};


InquiryPanel::InquiryPanel(BRect frame, LocalDevice* lDevice)
	:
	BWindow(frame, B_TRANSLATE("Scan for Bluetooth devices"), B_FLOATING_WINDOW,
	B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS,	B_ALL_WORKSPACES ),
	fMessenger(this),
 	fScanning(false),
	fLocalDevice(lDevice)

{
	fScanProgress = new BStatusBar("status", B_TRANSLATE("Scanning progress:"), "");
	activeColor = fScanProgress->BarColor();

	if (fLocalDevice == NULL)
		fLocalDevice = LocalDevice::GetLocalDevice();

	fMessage = new BTextView("description", B_WILL_DRAW);
	fMessage->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	fMessage->SetLowColor(fMessage->ViewColor());
	fMessage->MakeEditable(false);
	fMessage->MakeSelectable(false);

	fScanButton = new BButton("Scan", B_TRANSLATE("Scan"), new BMessage(kMsgInquiry), B_WILL_DRAW);

	fCancelButton = new BButton("cancel", B_TRANSLATE("Cancel"),
		new BMessage(kMsgCancel), B_WILL_DRAW);
	fCancelButton->SetEnabled(false);

	fAddButton = new BButton("add", B_TRANSLATE("Add device to list"),
		new BMessage(kMsgAddToRemoteList), B_WILL_DRAW);
	fAddButton->SetEnabled(false);

	fRemoteList = new BListView("AttributeList", B_SINGLE_SELECTION_LIST);
	fRemoteList->SetSelectionMessage(new BMessage(kMsgSelected));

	fScrollView = new BScrollView("ScrollView", fRemoteList, 0, false, true);
	fScrollView->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	if (fLocalDevice != NULL) {
		fMessage->SetText(B_TRANSLATE(
			"Make sure Bluetooth capabilities of the remote device are activated before "
			"starting the scan."));
		fScanButton->SetEnabled(true);
		fDiscoveryAgent = fLocalDevice->GetDiscoveryAgent();
		fDiscoveryListener = new PanelDiscoveryListener(this);

	} else {
		fMessage->SetText(B_TRANSLATE("This computer doesn't seem to have Bluetooth support."));
		fScanButton->SetEnabled(false);
	}

	fSecondsMessage = new BMessage(kMsgSecond);

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_SMALL_SPACING)
		.Add(fMessage, 0)
		.Add(fScanProgress, 10)
		.Add(fScrollView, 20)
		.AddGroup(B_HORIZONTAL, 10)
			.Add(fAddButton)
			.AddGlue()
			.Add(fCancelButton)
			.Add(fScanButton)
		.End()
	.End();
}


void
InquiryPanel::MessageReceived(BMessage* message)
{
	static float timer = 0; // expected time of the inquiry process
	static float scanningTime = 0;

	switch (message->what) {
		case kMsgInquiry:

			fDiscoveryAgent->StartInquiry(BT_GIAC, fDiscoveryListener, GetInquiryTime());

			timer = BT_BASE_INQUIRY_TIME * GetInquiryTime() + 1;
			// does it works as expected?
			fScanProgress->SetMaxValue(timer);

		break;

		case kMsgAddListDevice:
		{
			DeviceListItem* listItem;

			message->FindPointer("remoteItem", (void **)&listItem);

			fRemoteList->AddItem(listItem);
		}
		break;

		case kMsgAddToRemoteList:
		{
			message->PrintToStream();
			int32 index = fRemoteList->CurrentSelection(0);
			DeviceListItem* item = (DeviceListItem*) fRemoteList->RemoveItem(index);;

			BMessage message(kMsgAddToRemoteList);
			message.AddPointer("device", item);

			be_app->PostMessage(&message);
			// TODO: all others listitems can be deleted
		}
		break;

		case kMsgSelected:
			UpdateListStatus();
		break;

		case kMsgStart:
			fRemoteList->MakeEmpty();
			fScanProgress->Reset(B_TRANSLATE("Scanning progress:"));
			fScanProgress->SetTo(1);
			fScanProgress->SetTrailingText(B_TRANSLATE("Starting scan" B_UTF8_ELLIPSIS));
			fScanProgress->SetBarColor(activeColor);

			fAddButton->SetEnabled(false);
			fScanButton->SetEnabled(false);
			fCancelButton->SetEnabled(true);

			BMessageRunner::StartSending(fMessenger, fSecondsMessage, 1000000, timer);

			scanningTime = 1;
			fScanning = true;

		break;

		case kMsgFinish:
		case kMsgCancel:

			if (message->what == kMsgCancel)
				fDiscoveryAgent->CancelInquiry(fDiscoveryListener);

			fScanning = false;
			fCancelButton->SetEnabled(false);
			fScanProgress->SetTo(100);
			fScanProgress->SetBarColor(ui_color(B_PANEL_BACKGROUND_COLOR));
			fScanProgress->SetTrailingText(B_TRANSLATE("Scan finished"));
			fScanButton->SetEnabled(true);
			UpdateListStatus();

		break;

		case kMsgSecond:
			if (fScanning && scanningTime < timer) {
				// TODO should not be needed if SetMaxValue works...
				fScanProgress->SetTo(scanningTime * 100 / timer);

				BString text(B_TRANSLATE("%secs% remaining"));
				BString elapsedTime;
				BDurationFormat format;
				format.Format(elapsedTime, 0, (int)(timer - scanningTime) * 1000000LL);
				text.ReplaceFirst("%secs%", elapsedTime);
				fScanProgress->SetTrailingText(text);

				scanningTime = scanningTime + 1;
			}
		break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


void
InquiryPanel::UpdateListStatus(void)
{
	if (fRemoteList->CurrentSelection() < 0 || fScanning)
		fAddButton->SetEnabled(false);
	else
		fAddButton->SetEnabled(true);
}


bool
InquiryPanel::QuitRequested(void)
{
	if (fScanning)
		fDiscoveryAgent->CancelInquiry(fDiscoveryListener);

	if (fDiscoveryListener->Lock())
		fDiscoveryListener->Quit();

	return true;
}
