#pragma once

#include <Handler.h>
#include <Messenger.h>

#include <WaylandServerDefs.h>


class ServerHandler: public BHandler {
public:
	enum {
		closureSendMsg = 1,
	};

	ServerHandler();
	virtual ~ServerHandler() = default;

	void MessageReceived(BMessage *msg) final;
};


extern ServerHandler gServerHandler;
extern BMessenger gServerMessenger;


// Makes the dispatch thread send what other threads have queued.
void WaylandServerWake();
