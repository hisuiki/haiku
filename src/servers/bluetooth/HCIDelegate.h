/*
 * Copyright 2007-2008 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * Copyright 2008 Mika Lindqvist, monni1995_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef _HCIDELEGATE_H_
#define _HCIDELEGATE_H_

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <Path.h>

#include <bluetooth/HCI/btHCI_transport.h>

typedef void* raw_command;


class HCIDelegate {

	public:
		HCIDelegate(BPath* path)
		{
			fIdentifier = -1;
		}


		hci_id Id(void) const
		{
			return fIdentifier;
		}


		virtual ~HCIDelegate()
 		{

		}

		virtual status_t IssueCommand(raw_command rc, size_t size)=0; 
		virtual status_t Launch()=0;


		void FreeWindow(uint8 slots)
		{
		} 


		status_t QueueCommand(raw_command rc, size_t size) 
		{
			return IssueCommand(rc, size);
		}

	protected:

		hci_id fIdentifier;

	private:

};

#endif
