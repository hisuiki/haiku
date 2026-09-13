/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef INIT_HOME_LAYOUT_JOB_H
#define INIT_HOME_LAYOUT_JOB_H


#include <Job.h>
#include <String.h>
#include <StringList.h>


class InitHomeLayoutJob : public BSupportKit::BJob {
public:
								InitHomeLayoutJob();

protected:
	virtual	status_t			Execute();

private:
			bool				_NeedsMigration() const;
			status_t			_Migrate();
			status_t			_PrepareAccounts(const BString& oldPasswd,
									const BString& oldGroup, BString& passwd,
									BString& group) const;
			void				_EnsureAccounts() const;
			status_t			_MoveHomeEntries(BStringList& moved);
			void				_RestoreHomeEntries(const BStringList& moved);
};


#endif // INIT_HOME_LAYOUT_JOB_H
