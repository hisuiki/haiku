/*
 * Copyright 2001-2007, Ingo Weinhold, bonefish@users.sf.net.
 * Distributed under the terms of the MIT License.
 */

//!	An extended app_info.

#include "RosterAppInfo.h"

#include <new>
#include <string.h>

#include <Entry.h>
#include <OS.h>
#include <SupportDefs.h>


using std::nothrow;


// constructor
RosterAppInfo::RosterAppInfo()
	: app_info(),
	state(APP_STATE_UNREGISTERED),
	token(0),
	registration_time(0),
	session(0),
	uid(0)
{
}


// Init
void
RosterAppInfo::Init(thread_id thread, team_id team, port_id port, uint32 flags,
	const entry_ref *ref, const char *signature)
{
	this->thread = thread;
	SetTeam(team);
	this->port = port;
	this->flags = flags;
	BEntry entry(ref, true);
	if (entry.GetRef(&this->ref) != B_OK)
		this->ref = *ref;
	if (signature)
		strlcpy(this->signature, signature, B_MIME_TYPE_LENGTH);
	else
		this->signature[0] = '\0';
}


/*!	Sets the team the application runs in, and with it the session and the user
	the roster tells one application's clients from another's by.
*/
void
RosterAppInfo::SetTeam(team_id team)
{
	this->team = team;

	team_info teamInfo;
	if (team >= 0 && get_team_info(team, &teamInfo) == B_OK) {
		this->session = teamInfo.session_id;
		this->uid = teamInfo.uid;
	} else {
		this->session = 0;
		this->uid = 0;
	}
}


// Clone
RosterAppInfo *
RosterAppInfo::Clone() const
{
	RosterAppInfo *clone = new(nothrow) RosterAppInfo;
	if (!clone)
		return NULL;

	clone->Init(thread, team, port, flags, &ref, signature);
	clone->registration_time = registration_time;
	clone->session = session;
	clone->uid = uid;
	return clone;
}


// IsRunning
bool
RosterAppInfo::IsRunning() const
{
	team_info teamInfo;
	return get_team_info(team, &teamInfo) == B_OK;
}

