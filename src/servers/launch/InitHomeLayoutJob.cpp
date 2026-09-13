/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Moves a legacy /boot/home into /boot/home/user before other boot jobs run.
	On failure it restores the old layout and mounts packagefs at its old path.
*/


#include "InitHomeLayoutJob.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <set>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fs_attr.h>
#include <fs_volume.h>
#include <Node.h>
#include <OS.h>
#include <String.h>

#include <user_group.h>

#include "Utility.h"


using BSupportKit::BJob;


static const char* const kHomeRoot = "/boot/home";
static const char* const kDefaultUserHome = "/boot/home/user";
static const char* const kRootHome = "/boot/system/cache/root";
static const char* const kLegacyConfig = "/boot/home/config";
//! Service accounts added when upgrading an existing installation.
static const struct {
	const char*	name;
	int			id;
	const char*	realName;
	const char*	home;
} kServiceAccounts[] = {
	{ "_login", 2, "Login Service", "/boot/system/cache/login" },
	{ "_media", 3, "Media Service", "/boot/system/cache/media" }
};

static const char* const kServiceShell = "/bin/false";
static const char* const kBackupSuffix = ".single-user";
static const char* const kNewSuffix = ".multi-user";


static status_t
read_file(const char* path, BString& contents)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return errno;

	struct stat st;
	status_t status = B_OK;
	if (fstat(fd, &st) != 0)
		status = errno;
	else {
		char* buffer = contents.LockBuffer(st.st_size + 1);
		ssize_t bytesRead = buffer != NULL ? read(fd, buffer, st.st_size) : -1;
		if (bytesRead < 0)
			status = buffer != NULL ? errno : B_NO_MEMORY;
		contents.UnlockBuffer(bytesRead < 0 ? 0 : bytesRead);
	}

	close(fd);
	return status;
}


static status_t
write_file(const char* path, const BString& contents, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0)
		return errno;

	status_t status = B_OK;
	if (write(fd, contents.String(), contents.Length()) != contents.Length()
		|| fsync(fd) != 0) {
		status = errno;
	}

	close(fd);
	return status;
}


static status_t
mount_home_packagefs(const char* config)
{
	BString parameters;
	parameters << "packages " << config << "/packages; type home";
	dev_t device = fs_mount_volume(config, NULL, "packagefs", 0,
		parameters.String());
	return device < 0 ? device : B_OK;
}


static void
copy_attributes(const char* from, const char* to)
{
	BNode source(from);
	BNode target(to);
	if (source.InitCheck() != B_OK || target.InitCheck() != B_OK)
		return;

	char name[B_ATTR_NAME_LENGTH];
	while (source.GetNextAttrName(name) == B_OK) {
		attr_info info;
		if (source.GetAttrInfo(name, &info) != B_OK)
			continue;

		void* buffer = malloc(info.size);
		if (buffer == NULL)
			continue;
		ssize_t bytesRead = source.ReadAttr(name, info.type, 0, buffer,
			info.size);
		if (bytesRead >= 0)
			target.WriteAttr(name, info.type, 0, buffer, bytesRead);
		free(buffer);
	}
}


static bool
moved_link_target(const char* target, BString& movedTarget)
{
	size_t rootLength = strlen(kHomeRoot);
	if (strncmp(target, kHomeRoot, rootLength) != 0)
		return false;

	const char* rest = target + rootLength;
	if (rest[0] != '\0' && rest[0] != '/')
		return false;
	if (strcmp(rest, "/user") == 0 || strncmp(rest, "/user/", 6) == 0)
		return false;

	movedTarget = kDefaultUserHome;
	movedTarget << rest;
	return true;
}


static void
fix_links_in(const BString& path, dev_t device, int32& count)
{
	DIR* dir = opendir(path.String());
	if (dir == NULL)
		return;

	BString config(kDefaultUserHome);
	config << "/config";

	while (dirent* entry = readdir(dir)) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;

		BString child(path);
		child << "/" << entry->d_name;
		struct stat st;
		if (lstat(child.String(), &st) != 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			// Enter packagefs only to reach its writable shine-through entries.
			if (st.st_dev == device || child == config)
				fix_links_in(child, device, count);
			continue;
		}
		if (!S_ISLNK(st.st_mode) || st.st_dev != device)
			continue;

		char target[B_PATH_NAME_LENGTH];
		ssize_t length = readlink(child.String(), target, sizeof(target) - 1);
		if (length < 0)
			continue;
		target[length] = '\0';

		BString movedTarget;
		if (!moved_link_target(target, movedTarget))
			continue;

		BString temporary(child);
		temporary << kNewSuffix;
		if (symlink(movedTarget.String(), temporary.String()) == 0
			&& rename(temporary.String(), child.String()) == 0) {
			count++;
		} else
			unlink(temporary.String());
	}

	closedir(dir);
}


//! Rewrites absolute links into the legacy home in the background.
static int32
fix_home_links(void*)
{
	struct stat st;
	if (lstat(kDefaultUserHome, &st) != 0)
		return 0;

	int32 count = 0;
	fix_links_in(kDefaultUserHome, st.st_dev, count);
	debug_printf("launch_daemon: pointed %" B_PRId32 " symbolic links at the "
		"moved home\n", count);
	return 0;
}


static status_t
set_home_owner(const BString& path, dev_t homeDevice, uid_t uid, gid_t gid)
{
	struct stat st;
	if (lstat(path.String(), &st) != 0)
		return errno;

	BString config(kDefaultUserHome);
	config << "/config";
	bool packageFSMount = path == config;
	if (st.st_dev != homeDevice && !packageFSMount)
		return B_OK;

	if (!packageFSMount && lchown(path.String(), uid, gid) != 0)
		return errno;
	if (!S_ISDIR(st.st_mode))
		return B_OK;

	DIR* directory = opendir(path.String());
	if (directory == NULL)
		return errno;

	status_t status = B_OK;
	while (dirent* entry = readdir(directory)) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;

		BString child(path);
		child << "/" << entry->d_name;
		status_t childStatus = set_home_owner(child, homeDevice, uid, gid);
		if (childStatus != B_OK)
			status = childStatus;
	}
	closedir(directory);
	return status;
}


static status_t
default_user_ids(uid_t& uid, gid_t& gid)
{
	BString passwd;
	status_t status = read_file(BPrivate::kPasswdFile, passwd);
	if (status != B_OK)
		return status;

	BStringList lines;
	passwd.Split("\n", true, lines);
	for (int32 i = 0; i < lines.CountStrings(); i++) {
		BStringList fields;
		lines.StringAt(i).Split(":", false, fields);
		if (fields.CountStrings() >= 7 && fields.StringAt(5) == kDefaultUserHome) {
			uid = atoi(fields.StringAt(2).String());
			gid = atoi(fields.StringAt(3).String());
			return B_OK;
		}
	}

	return B_ENTRY_NOT_FOUND;
}


// #pragma mark -


InitHomeLayoutJob::InitHomeLayoutJob()
	:
	BJob("init home layout")
{
}


status_t
InitHomeLayoutJob::Execute()
{
	if (_NeedsMigration()) {
		status_t status = B_READ_ONLY_DEVICE;
		if (!Utility::IsReadOnlyVolume(kHomeRoot))
			status = _Migrate();

		if (status != B_OK) {
			debug_printf("launch_daemon: could not move the single-user home "
				"to %s: %s. Using it where it is.\n", kDefaultUserHome,
				strerror(status));
			status = mount_home_packagefs(kLegacyConfig);
			if (status != B_OK) {
				debug_printf("launch_daemon: could not mount the home "
					"packagefs at %s: %s\n", kLegacyConfig, strerror(status));
			}
		}
	}

	_EnsureAccounts();

	if (mkdir(kRootHome, 0700) != 0 && errno != EEXIST) {
		debug_printf("launch_daemon: could not create %s: %s\n", kRootHome,
			strerror(errno));
	}
	chmod(kRootHome, 0700);

	struct stat st;
	uid_t uid;
	gid_t gid;
	if (lstat(kDefaultUserHome, &st) == 0 && st.st_uid == 0
		&& default_user_ids(uid, gid) == B_OK && uid != 0) {
		status_t status = set_home_owner(kDefaultUserHome, st.st_dev, uid, gid);
		if (status != B_OK) {
			debug_printf("launch_daemon: could not give %s to uid %d: %s\n",
				kDefaultUserHome, (int)uid, strerror(status));
		}
	}

	setenv("HOME", kRootHome, true);

	return B_OK;
}


bool
InitHomeLayoutJob::_NeedsMigration() const
{
	struct stat st;
	if (lstat(kDefaultUserHome, &st) == 0)
		return false;

	BString packages(kLegacyConfig);
	packages << "/packages";
	if (stat(packages.String(), &st) != 0 || !S_ISDIR(st.st_mode))
		return false;

	// Only a single-user kernel mounts packagefs at the legacy path.
	struct stat home;
	struct stat config;
	return stat(kHomeRoot, &home) == 0 && stat(kLegacyConfig, &config) == 0
		&& home.st_dev == config.st_dev;
}


status_t
InitHomeLayoutJob::_Migrate()
{
	BString oldPasswd;
	BString oldGroup;
	status_t status = read_file(BPrivate::kPasswdFile, oldPasswd);
	if (status == B_OK)
		status = read_file(BPrivate::kGroupFile, oldGroup);
	if (status != B_OK)
		return status;

	BString passwd;
	BString group;
	status = _PrepareAccounts(oldPasswd, oldGroup, passwd, group);
	if (status != B_OK)
		return status;

	// Replace the account files only after the home has moved.
	BString passwdBackup = BString(BPrivate::kPasswdFile) << kBackupSuffix;
	BString groupBackup = BString(BPrivate::kGroupFile) << kBackupSuffix;
	BString passwdNew = BString(BPrivate::kPasswdFile) << kNewSuffix;
	BString groupNew = BString(BPrivate::kGroupFile) << kNewSuffix;
	status = write_file(passwdBackup, oldPasswd, 0644);
	if (status == B_OK)
		status = write_file(groupBackup, oldGroup, 0644);
	if (status == B_OK)
		status = write_file(passwdNew, passwd, 0644);
	if (status == B_OK)
		status = write_file(groupNew, group, 0644);
	if (status == B_OK && mkdir(kDefaultUserHome, 0755) != 0)
		status = errno;
	if (status != B_OK) {
		unlink(passwdNew);
		unlink(groupNew);
		return status;
	}
	copy_attributes(kHomeRoot, kDefaultUserHome);

	BStringList moved;
	status = _MoveHomeEntries(moved);
	if (status == B_OK && rename(groupNew, BPrivate::kGroupFile) != 0)
		status = errno;
	if (status == B_OK && rename(passwdNew, BPrivate::kPasswdFile) != 0) {
		status = errno;
		write_file(BPrivate::kGroupFile, oldGroup, 0644);
	}
	if (status != B_OK) {
		_RestoreHomeEntries(moved);
		rmdir(kDefaultUserHome);
		unlink(passwdNew);
		unlink(groupNew);
		return status;
	}
	sync();

	debug_printf("launch_daemon: moved the single-user home into %s "
		"(%" B_PRId32 " entries); account files saved as *%s\n",
		kDefaultUserHome, moved.CountStrings(), kBackupSuffix);

	BString config(kDefaultUserHome);
	config << "/config";
	status = mount_home_packagefs(config);
	if (status != B_OK) {
		debug_printf("launch_daemon: could not mount the home packagefs at "
			"%s: %s\n", config.String(), strerror(status));
	}

	thread_id thread = spawn_thread(&fix_home_links, "fix home links",
		B_LOW_PRIORITY, NULL);
	if (thread >= 0)
		resume_thread(thread);

	return B_OK;
}


status_t
InitHomeLayoutJob::_PrepareAccounts(const BString& oldPasswd,
	const BString& oldGroup, BString& passwd, BString& group) const
{
	group = oldGroup;

	bool rootFound = false;
	BStringList lines;
	oldPasswd.Split("\n", true, lines);
	for (int32 i = 0; i < lines.CountStrings(); i++) {
		BString line = lines.StringAt(i);
		BStringList fields;
		line.Split(":", false, fields);
		if (fields.CountStrings() >= 7 && atoi(fields.StringAt(2).String()) == 0
			&& (fields.StringAt(5) == kHomeRoot
				|| fields.StringAt(5) == BString(kHomeRoot) << "/")) {
			fields.Replace(5, kDefaultUserHome);
			line = fields.Join(":");
			rootFound = true;
		}
		passwd << line << "\n";
	}

	return rootFound ? B_OK : B_ENTRY_NOT_FOUND;
}


void
InitHomeLayoutJob::_EnsureAccounts() const
{
	BString oldPasswd;
	BString oldGroup;
	if (read_file(BPrivate::kPasswdFile, oldPasswd) != B_OK
		|| read_file(BPrivate::kGroupFile, oldGroup) != B_OK) {
		return;
	}

	std::set<int> usedIDs;
	std::set<BString> users;
	std::set<BString> groups;
	BStringList passwdLines;
	oldPasswd.Split("\n", true, passwdLines);
	int32 defaultUser = -1;
	for (int32 i = 0; i < passwdLines.CountStrings(); i++) {
		BStringList fields;
		passwdLines.StringAt(i).Split(":", false, fields);
		if (fields.CountStrings() >= 7) {
			usedIDs.insert(atoi(fields.StringAt(2).String()));
			users.insert(fields.StringAt(0));
			if (fields.StringAt(5) == kDefaultUserHome
				&& atoi(fields.StringAt(2).String()) == 0) {
				defaultUser = i;
			}
		}
	}
	BStringList groupLines;
	oldGroup.Split("\n", true, groupLines);
	for (int32 i = 0; i < groupLines.CountStrings(); i++) {
		BStringList fields;
		groupLines.StringAt(i).Split(":", false, fields);
		if (fields.CountStrings() >= 3) {
			usedIDs.insert(atoi(fields.StringAt(2).String()));
			groups.insert(fields.StringAt(0));
		}
	}

	bool changed = false;
	if (defaultUser >= 0) {
		int uid = 1000;
		while (usedIDs.find(uid) != usedIDs.end())
			uid++;

		BStringList fields;
		passwdLines.StringAt(defaultUser).Split(":", false, fields);
		BString uidString;
		uidString << uid;
		fields.Replace(2, uidString);
		fields.Replace(3, "100");
		passwdLines.Replace(defaultUser, fields.Join(":"));
		usedIDs.insert(uid);
		changed = true;
	}

	bool hasRootUser = false;
	for (int32 i = 0; i < passwdLines.CountStrings(); i++) {
		BStringList fields;
		passwdLines.StringAt(i).Split(":", false, fields);
		if (fields.CountStrings() >= 7
			&& atoi(fields.StringAt(2).String()) == 0) {
			hasRootUser = true;
			break;
		}
	}
	if (!hasRootUser) {
		BString name("root");
		for (int suffix = 2; users.find(name) != users.end(); suffix++)
			name.SetToFormat("root%d", suffix);
		passwdLines.Add(BString(name) << ":x:0:0:System Administrator:"
			<< kRootHome << ":/bin/bash");
		users.insert(name);
		changed = true;
	}

	BString passwd = passwdLines.Join("\n");
	BString group(oldGroup);
	if (!passwd.IsEmpty() && !passwd.EndsWith("\n"))
		passwd << "\n";
	if (!group.IsEmpty() && !group.EndsWith("\n"))
		group << "\n";

	for (size_t i = 0; i < sizeof(kServiceAccounts) / sizeof(kServiceAccounts[0]);
			i++) {
		const char* name = kServiceAccounts[i].name;
		if (users.find(name) != users.end())
			continue;

		int id = kServiceAccounts[i].id;
		if (usedIDs.find(id) != usedIDs.end()) {
			// Taken on this system: any free system ID will do.
			id = -1;
			for (int candidate = 2; candidate < 100 && id < 0; candidate++) {
				if (usedIDs.find(candidate) == usedIDs.end())
					id = candidate;
			}
			if (id < 0) {
				debug_printf("launch_daemon: no free system ID for the \"%s\" "
					"account\n", name);
				continue;
			}
		}
		usedIDs.insert(id);

		if (groups.find(name) == groups.end())
			group << name << ":x:" << id << ":\n";
		passwd << name << ":x:" << id << ":" << id << ":"
			<< kServiceAccounts[i].realName << ":" << kServiceAccounts[i].home
			<< ":" << kServiceShell << "\n";
		changed = true;

		debug_printf("launch_daemon: added the \"%s\" account (%d)\n", name,
			id);
	}
	if (!changed)
		return;

	BString passwdNew = BString(BPrivate::kPasswdFile) << kNewSuffix;
	BString groupNew = BString(BPrivate::kGroupFile) << kNewSuffix;
	if (write_file(passwdNew, passwd, 0644) == B_OK
		&& write_file(groupNew, group, 0644) == B_OK
		&& rename(groupNew, BPrivate::kGroupFile) == 0
		&& rename(passwdNew, BPrivate::kPasswdFile) == 0) {
		sync();
		return;
	}

	debug_printf("launch_daemon: could not update the accounts: %s\n",
		strerror(errno));
	unlink(passwdNew);
	unlink(groupNew);
}


status_t
InitHomeLayoutJob::_MoveHomeEntries(BStringList& moved)
{
	// Collect the names first: renaming entries out of a directory while
	// reading it can make the directory skip some.
	DIR* dir = opendir(kHomeRoot);
	if (dir == NULL)
		return errno;

	BStringList names;
	BString userName(kDefaultUserHome + strlen(kHomeRoot) + 1);
	while (dirent* entry = readdir(dir)) {
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0
			&& userName != entry->d_name) {
			names.Add(entry->d_name);
		}
	}
	closedir(dir);

	for (int32 i = 0; i < names.CountStrings(); i++) {
		BString from = BString(kHomeRoot) << "/" << names.StringAt(i);
		BString to = BString(kDefaultUserHome) << "/" << names.StringAt(i);
		if (rename(from.String(), to.String()) != 0) {
			status_t status = errno;
			debug_printf("launch_daemon: could not move %s to %s: %s\n",
				from.String(), to.String(), strerror(status));
			return status;
		}
		moved.Add(names.StringAt(i));
	}

	return B_OK;
}


void
InitHomeLayoutJob::_RestoreHomeEntries(const BStringList& moved)
{
	for (int32 i = 0; i < moved.CountStrings(); i++) {
		BString from = BString(kDefaultUserHome) << "/" << moved.StringAt(i);
		BString to = BString(kHomeRoot) << "/" << moved.StringAt(i);
		rename(from.String(), to.String());
	}
}
