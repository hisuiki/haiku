/*
 * Copyright 2008, Ingo Weinhold, ingo_weinhold@gmx.de. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include "multiuser_utils.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <fs_attr.h>

#include <AutoDeleterPosix.h>

#include <user_group.h>


//! The administrator's template overrides the package-provided template.
static const char* const kHomeTemplates[] = {
	"/boot/system/data/home_template",
	"/boot/system/settings/home_template"
};

static const char* const kHomeDirectories[] = {
	"Desktop", "config", "config/cache", "config/non-packaged",
	"config/packages", "config/settings", "config/var"
};


status_t
read_password(const char* prompt, char* password, size_t bufferSize,
	bool useStdio)
{
	FILE* in = stdin;
	FILE* out = stdout;

	// open tty
	FileCloser tty;
	if (!useStdio) {
// TODO: Open tty with O_NOCTTY!
		tty.SetTo(fopen("/dev/tty", "w+"));
		if (!tty.IsSet()) {
			fprintf(stderr, "Error: Failed to open tty: %s\n",
				strerror(errno));
			return errno;
		}

		in = tty.Get();
		out = tty.Get();
	}

	// disable echo
	int inFD = fileno(in);
	struct termios termAttrs;
	if (tcgetattr(inFD, &termAttrs) != 0) {
		fprintf(in, "Error: Failed to get tty attributes: %s\n",
			strerror(errno));
		return errno;
	}

	tcflag_t localFlags = termAttrs.c_lflag;
	termAttrs.c_lflag &= ~ECHO;

	if (tcsetattr(inFD, TCSANOW, &termAttrs) != 0) {
		fprintf(in, "Error: Failed to set tty attributes: %s\n",
			strerror(errno));
		return errno;
	}

	status_t error = B_OK;

	// prompt and read pwd
	fputs(prompt, out);
	fflush(out);

	if (fgets(password, bufferSize, in) == NULL) {
		fprintf(out, "\nError: Failed to read from tty: %s\n",
			strerror(errno));
		error = errno != 0 ? errno : B_ERROR;
	} else
		fputc('\n', out);

	// chop off trailing newline
	if (error == B_OK) {
		size_t len = strlen(password);
		if (len > 0 && password[len - 1] == '\n')
			password[len - 1] = '\0';
	}

	// restore the terminal attributes
	termAttrs.c_lflag = localFlags;
	tcsetattr(inFD, TCSANOW, &termAttrs);

	return error;
}


bool
verify_password(passwd* passwd, spwd* spwd, const char* plainPassword)
{
	if (passwd == NULL)
		return false;

	// check whether we need to check the shadow password
	const char* requiredPassword = passwd->pw_passwd;
	if (strcmp(requiredPassword, "x") == 0) {
		if (spwd == NULL) {
			// Mmh, we're suppose to check the shadow password, but we don't
			// have it. Bail out.
			return false;
		}

		requiredPassword = spwd->sp_pwdp;
	}

	// If no password is required, we're done.
	if (requiredPassword == NULL || requiredPassword[0] == '\0') {
		if (plainPassword == NULL || plainPassword[0] == '\0')
			return true;

		return false;
	}

	// crypt and check it
	char* encryptedPassword = crypt(plainPassword, requiredPassword);

	return (strcmp(encryptedPassword, requiredPassword) == 0);
}


/*!	Checks whether the user needs to authenticate with a password, and, if
	necessary, asks for it, and checks it.
	\a passwd must always be given, \a spwd only if there exists an entry
	for the user.
*/
status_t
authenticate_user(const char* prompt, passwd* passwd, spwd* spwd, int maxTries,
	bool useStdio)
{
	// check whether a password is need at all
	if (verify_password(passwd, spwd, ""))
		return B_OK;

	while (true) {
		// prompt the user for the password
		char plainPassword[MAX_SHADOW_PWD_PASSWORD_LEN];
		status_t error = read_password(prompt, plainPassword,
			sizeof(plainPassword), useStdio);
		if (error != B_OK)
			return error;

		// check it
		bool ok = verify_password(passwd, spwd, plainPassword);
		explicit_bzero(plainPassword, sizeof(plainPassword));
		if (ok)
			return B_OK;

		fprintf(stderr, "Incorrect password.\n");
		if (--maxTries <= 0)
			return B_PERMISSION_DENIED;
	}
}


status_t
setup_environment(struct passwd* passwd, bool preserveEnvironment, bool chngdir)
{
	const char* term = getenv("TERM");
	if (!preserveEnvironment) {
		static char *empty[1];
		environ = empty;
	}

	// always preserve $TERM
	if (term != NULL)
		setenv("TERM", term, false);
	if (passwd->pw_shell)
		setenv("SHELL", passwd->pw_shell, true);
	if (passwd->pw_dir)
		setenv("HOME", passwd->pw_dir, true);

	setenv("USER", passwd->pw_name, true);

	pid_t pid = getpid();
	// Noninteractive SSH uses a pipe here, not a terminal.
	if (isatty(STDIN_FILENO)) {
		if (ioctl(STDIN_FILENO, TIOCSPGRP, &pid) != 0)
			return errno;
	}

	if (setresgid(passwd->pw_gid, passwd->pw_gid, passwd->pw_gid) != 0)
		return errno;

	if (initgroups(passwd->pw_name, passwd->pw_gid) != 0)
		return errno;

	if (setresuid(passwd->pw_uid, passwd->pw_uid, passwd->pw_uid) != 0)
		return errno;

	if (chngdir) {
		const char* home = getenv("HOME");
		if (home == NULL)
			return B_ENTRY_NOT_FOUND;

		if (chdir(home) != 0)
			return errno;
	}

	return B_OK;
}


// #pragma mark - home directories


static void
copy_attributes(int sourceFD, int destinationFD)
{
	DIR* attributes = fs_fopen_attr_dir(sourceFD);
	if (attributes == NULL)
		return;

	while (dirent* entry = fs_read_attr_dir(attributes)) {
		// Do not copy packagefs bookkeeping into the user's files.
		if (strcmp(entry->d_name, "SYS:PACKAGE") == 0
			|| strcmp(entry->d_name, "SYS:PACKAGE_FILE") == 0) {
			continue;
		}

		attr_info info;
		if (fs_stat_attr(sourceFD, entry->d_name, &info) != 0)
			continue;

		void* buffer = malloc(info.size);
		if (buffer == NULL)
			continue;

		ssize_t bytesRead = fs_read_attr(sourceFD, entry->d_name, info.type, 0,
			buffer, info.size);
		if (bytesRead >= 0) {
			fs_write_attr(destinationFD, entry->d_name, info.type, 0, buffer,
				bytesRead);
		}
		free(buffer);
	}

	fs_close_attr_dir(attributes);
}


static status_t
copy_file(const char* from, const char* to, const struct stat& fromStat,
	uid_t uid, gid_t gid)
{
	int source = open(from, O_RDONLY);
	if (source < 0)
		return errno;
	FileDescriptorCloser sourceCloser(source);

	// Template files can be read-only in packagefs; their copies are writable.
	int destination = open(to, O_WRONLY | O_CREAT | O_TRUNC,
		(fromStat.st_mode & 0777) | S_IRUSR | S_IWUSR);
	if (destination < 0)
		return errno;
	FileDescriptorCloser destinationCloser(destination);

	char buffer[64 * 1024];
	ssize_t bytesRead;
	while ((bytesRead = read(source, buffer, sizeof(buffer))) > 0) {
		if (write(destination, buffer, bytesRead) != bytesRead)
			return errno;
	}
	if (bytesRead < 0)
		return errno;

	copy_attributes(source, destination);
	if (fchown(destination, uid, gid) != 0)
		return errno;

	return B_OK;
}


//! Recursively merges a template while preserving attributes.
static status_t
copy_entry(const char* from, const char* to, uid_t uid, gid_t gid)
{
	struct stat fromStat;
	if (lstat(from, &fromStat) != 0)
		return errno;

	if (S_ISLNK(fromStat.st_mode)) {
		char target[PATH_MAX];
		ssize_t length = readlink(from, target, sizeof(target) - 1);
		if (length < 0)
			return errno;
		target[length] = '\0';

		if (symlink(target, to) != 0 && errno != EEXIST)
			return errno;
		lchown(to, uid, gid);
		return B_OK;
	}

	if (!S_ISDIR(fromStat.st_mode))
		return copy_file(from, to, fromStat, uid, gid);

	if (mkdir(to, 0755) != 0 && errno != EEXIST)
		return errno;

	int destination = open(to, O_RDONLY);
	if (destination >= 0) {
		int source = open(from, O_RDONLY);
		if (source >= 0) {
			copy_attributes(source, destination);
			close(source);
		}
		fchown(destination, uid, gid);
		close(destination);
	}

	DIR* directory = opendir(from);
	if (directory == NULL)
		return errno;

	status_t status = B_OK;
	while (dirent* entry = readdir(directory)) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;

		char fromEntry[PATH_MAX];
		char toEntry[PATH_MAX];
		if (snprintf(fromEntry, sizeof(fromEntry), "%s/%s", from,
					entry->d_name) >= (int)sizeof(fromEntry)
			|| snprintf(toEntry, sizeof(toEntry), "%s/%s", to, entry->d_name)
					>= (int)sizeof(toEntry)) {
			status = B_NAME_TOO_LONG;
			continue;
		}

		status_t entryStatus = copy_entry(fromEntry, toEntry, uid, gid);
		if (entryStatus != B_OK)
			status = entryStatus;
	}

	closedir(directory);
	return status;
}


status_t
create_user_home(const char* home, uid_t uid, gid_t gid, bool fromTemplate)
{
	if (home == NULL || home[0] != '/')
		return B_BAD_VALUE;

	if (mkdir(home, 0700) != 0 && errno != EEXIST)
		return errno;
	if (chown(home, uid, gid) != 0)
		return errno;

	bool copiedTemplate = false;
	for (size_t i = 0; fromTemplate
			&& i < sizeof(kHomeTemplates) / sizeof(kHomeTemplates[0]); i++) {
		struct stat st;
		if (stat(kHomeTemplates[i], &st) != 0 || !S_ISDIR(st.st_mode))
			continue;

		status_t status = copy_entry(kHomeTemplates[i], home, uid, gid);
		if (status != B_OK)
			return status;
		copiedTemplate = true;
	}
	if (!copiedTemplate) {
		for (size_t i = 0; i < sizeof(kHomeDirectories) / sizeof(kHomeDirectories[0]);
				i++) {
			char path[PATH_MAX];
			if (snprintf(path, sizeof(path), "%s/%s", home, kHomeDirectories[i])
					>= (int)sizeof(path)) {
				return B_NAME_TOO_LONG;
			}
			if (mkdir(path, 0755) != 0 && errno != EEXIST)
				return errno;
			if (chown(path, uid, gid) != 0)
				return errno;
		}
	}

	char firstLoginPath[PATH_MAX];
	if (snprintf(firstLoginPath, sizeof(firstLoginPath),
			"%s/config/settings/first_login", home) < (int)sizeof(firstLoginPath)) {
		struct stat st;
		if (stat(firstLoginPath, &st) != 0) {
			char replicantsPath[PATH_MAX];
			snprintf(replicantsPath, sizeof(replicantsPath),
				"%s/config/settings/deskbar/replicants", home);
			if (stat(replicantsPath, &st) != 0) {
				int fd = open(firstLoginPath, O_WRONLY | O_CREAT | O_EXCL, 0644);
				if (fd >= 0) {
					write(fd, "1\n", 2);
					fchown(fd, uid, gid);
					close(fd);
				}
			}
		}
	}

	return B_OK;
}
