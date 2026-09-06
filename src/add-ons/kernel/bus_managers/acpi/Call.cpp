/*
 * Copyright 2022, Jérôme Duval. All rights reserved.
 *
 * Distributed under the terms of the MIT License.
 */


#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <Drivers.h>
#include <KernelExport.h>
#include <OS.h>

#include "ACPIPrivate.h"


#define	TRACE(x...)			//dprintf("acpi_call: " x)
#define TRACE_ALWAYS(x...)	dprintf("acpi_call: " x)
#define CALLED() 			TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


typedef struct {
	device_node *node;
	acpi_root_info	*acpi;
	void	*acpi_cookie;
} acpi_call_device_info;


#define MAX_ACPI_CALL_ARGS			7
#define MAX_ACPI_CALL_DATA_SIZE		(64 * 1024)


struct acpi_call_desc
{
	char*			path;
	acpi_objects	args;
	acpi_status	retval;
	acpi_data	result;
	acpi_size	reslen;
};


//	#pragma mark - device module API


static status_t
acpi_call_init_device(void* _node, void** _cookie)
{
	CALLED();
	device_node *node = (device_node *)_node;

	acpi_call_device_info* device = (acpi_call_device_info*)calloc(1, sizeof(acpi_call_device_info));
	if (device == NULL)
		return B_NO_MEMORY;

	device->node = node;
	status_t err = gDeviceManager->get_driver(node, (driver_module_info **)&device->acpi,
		(void **)&device->acpi_cookie);
	if (err != B_OK) {
		free(device);
		return err;
	}

	*_cookie = device;
	return err;
}


static void
acpi_call_uninit_device(void* _cookie)
{
	CALLED();
	acpi_call_device_info* device = (acpi_call_device_info*)_cookie;
	free(device);
}


static status_t
acpi_call_open(void* _device, const char* path, int openMode, void** _cookie)
{
	CALLED();
	acpi_call_device_info* device = (acpi_call_device_info*)_device;

	*_cookie = device;
	return B_OK;
}


static status_t
acpi_call_read(void *cookie, off_t position, void *buffer, size_t *numBytes)
{
	TRACE("read(%p, %" B_PRIdOFF", %p, %lu)\n", cookie, position, buffer, *numBytes);
	return B_ERROR;
}


static status_t
acpi_call_write(void *cookie, off_t position, const void *buffer,
	size_t *numBytes)
{
	TRACE("write(%p, %" B_PRIdOFF", %p, %lu)\n", cookie, position, buffer, *numBytes);
	return B_ERROR;
}


void
acpi_call_fixup_pointers(acpi_object_type *p, void *target)
{
	CALLED();
	switch (p->object_type)
	{
	case ACPI_TYPE_STRING:
		p->string.string = (char*)((uint8*)(p->string.string) - (uint8*)p + (uint8*)target);
		break;
	case ACPI_TYPE_BUFFER:
		p->buffer.buffer = (void*)((uint8*)(p->buffer.buffer) - (uint8*)p + (uint8*)target);
		break;
	}
}


/*!	Frees an argument list previously copied in from userland. */
static void
acpi_call_free_args(acpi_object_type* objects, uint32 count)
{
	if (objects == NULL)
		return;

	for (uint32 i = 0; i < count; i++) {
		switch (objects[i].object_type) {
			case ACPI_TYPE_STRING:
				free(objects[i].string.string);
				break;
			case ACPI_TYPE_BUFFER:
				free(objects[i].buffer.buffer);
				break;
		}
	}

	free(objects);
}


/*!	Copies a method argument list from userland into kernel memory.

	The objects themselves, as well as the data referenced by string and
	buffer objects, live in userland and must not be handed to ACPICA
	directly.
*/
static status_t
acpi_call_copy_args_from_user(acpi_objects* args)
{
	uint32 count = args->count;
	if (count == 0) {
		args->pointer = NULL;
		return B_OK;
	}
	if (count > MAX_ACPI_CALL_ARGS)
		return B_BAD_VALUE;

	acpi_object_type* objects = (acpi_object_type*)calloc(count,
		sizeof(acpi_object_type));
	if (objects == NULL)
		return B_NO_MEMORY;

	if (user_memcpy(objects, args->pointer,
			count * sizeof(acpi_object_type)) != B_OK) {
		free(objects);
		return B_BAD_ADDRESS;
	}

	// Deep copy the payload of the objects which reference user memory.
	for (uint32 i = 0; i < count; i++) {
		void* data = NULL;
		size_t size = 0;

		switch (objects[i].object_type) {
			case ACPI_TYPE_STRING:
				data = objects[i].string.string;
				size = (size_t)objects[i].string.len;
				break;
			case ACPI_TYPE_BUFFER:
				data = objects[i].buffer.buffer;
				size = (size_t)objects[i].buffer.length;
				break;
			default:
				// Anything else is self-contained, or not supported as an
				// argument coming from userland.
				continue;
		}

		if (data == NULL || size == 0 || size > MAX_ACPI_CALL_DATA_SIZE) {
			acpi_call_free_args(objects, i);
			return B_BAD_VALUE;
		}

		// Strings get an extra terminating null byte, buffers are used as is.
		bool isString = objects[i].object_type == ACPI_TYPE_STRING;
		void* copy = calloc(1, isString ? size + 1 : size);
		if (copy == NULL) {
			acpi_call_free_args(objects, i);
			return B_NO_MEMORY;
		}
		if (user_memcpy(copy, data, size) != B_OK) {
			free(copy);
			acpi_call_free_args(objects, i);
			return B_BAD_ADDRESS;
		}

		if (isString)
			objects[i].string.string = (char*)copy;
		else
			objects[i].buffer.buffer = copy;
	}

	args->pointer = objects;
	return B_OK;
}


static status_t
acpi_call_control(void *_device, uint32 op, void *buffer, size_t length)
{
	TRACE("control(%p, %" B_PRIu32 ", %p, %lu)\n", _device, op, buffer, length);
	acpi_call_device_info* device = (acpi_call_device_info*)_device;

	if (op == 'ACCA') {
		struct acpi_call_desc params;
		char path[1024];
		if (user_memcpy(&params, buffer, sizeof(params)) != B_OK)
			return B_BAD_ADDRESS;
		if (user_strlcpy(path, params.path, sizeof(path)) < 0)
			return B_BAD_ADDRESS;

		status_t status = acpi_call_copy_args_from_user(&params.args);
		if (status != B_OK)
			return status;

		acpi_data result;
		result.length = ACPI_ALLOCATE_BUFFER;
		result.pointer = NULL;

		acpi_status retval = device->acpi->evaluate_method(NULL, path, &params.args, &result);
		acpi_call_free_args(params.args.pointer, params.args.count);
		if (retval == 0) {
			if (result.pointer != NULL) {
				if (params.result.pointer != NULL) {
					params.result.length = min_c(params.result.length, result.length);
					if (result.length >= sizeof(acpi_object_type))
						acpi_call_fixup_pointers((acpi_object_type*)(result.pointer), params.result.pointer);

					if (user_memcpy(params.result.pointer, result.pointer, params.result.length) != B_OK
						|| user_memcpy(buffer, &params, sizeof(params)) != B_OK) {
						return B_BAD_ADDRESS;
					}
				}
				free(result.pointer);
			}
		}
		return B_OK;
	}

	return B_ERROR;
}


static status_t
acpi_call_close(void *cookie)
{
	TRACE("close(%p)\n", cookie);
	return B_OK;
}


static status_t
acpi_call_free(void *cookie)
{
	TRACE("free(%p)\n", cookie);
	return B_OK;
}


//	#pragma mark -


struct device_module_info gAcpiCallDeviceModule = {
	{
		ACPI_CALL_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	acpi_call_init_device,
	acpi_call_uninit_device,
	NULL, // remove,

	acpi_call_open,
	acpi_call_close,
	acpi_call_free,
	acpi_call_read,
	acpi_call_write,
	NULL,	// io
	acpi_call_control,

	NULL,	// select
	NULL,	// deselect
};

