/*
 * Copyright 2015, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef UTILITY_H
#define UTILITY_H


#include <String.h>


namespace Utility {
	bool IsReadOnlyVolume(dev_t device);
	bool IsReadOnlyVolume(const char* path);
	bool IsRemovableVolume(const char* path);

	status_t BlockMedia(const char* path, bool block);
	status_t EjectMedia(const char* path);

	BString TranslatePath(const char* path);
}


#endif // UTILITY_H
