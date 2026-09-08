/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Emi <emi@nuka.works>
 */

/*!	Intel VT-d (DMA remapping) support.

	Haiku does not drive the IOMMU, but the firmware of recent machines leaves
	the remapping hardware in a state which blocks device DMA before an OS
	takes over: the protected memory regions of the pre-boot DMA protection
	stay armed, and any device reading or writing host memory inside them gets
	a hardware error instead of data. On a ThinkPad P50 this is what keeps the
	Intel Wireless 8260 from loading its firmware.

	This driver walks the DMAR table, reports the state of each remapping
	unit, and lifts the protection so that DMA works the way the rest of the
	system expects.
*/


#include <ACPI.h>
#include <Drivers.h>
#include <KernelExport.h>
#include <module.h>

#include <string.h>


#define TRACE(x...)	dprintf("vtd: " x)


/* DMAR ACPI table, see the Intel VT-d specification, chapter 8. */

typedef struct {
	char						signature[4];
	uint32						length;
	uint8						revision;
	uint8						checksum;
	char						oem_id[6];
	char						oem_table_id[8];
	uint32						oem_revision;
	char						creator_id[4];
	uint32						creator_revision;
} _PACKED acpi_table_header;

typedef struct {
	acpi_table_header			header;
	uint8						host_address_width;
	uint8						flags;
	uint8						reserved[10];
} _PACKED dmar_table;

typedef struct {
	uint16						type;
	uint16						length;
} _PACKED dmar_entry;

typedef struct {
	dmar_entry					entry;
	uint8						flags;
	uint8						size;
	uint16						segment;
	uint64						address;
} _PACKED dmar_drhd;

#define DMAR_TYPE_DRHD			0

/* Remapping hardware registers, see chapter 10. */

#define DMAR_VER_REG			0x00
#define DMAR_CAP_REG			0x08
#define DMAR_ECAP_REG			0x10
#define DMAR_GCMD_REG			0x18
#define DMAR_GSTS_REG			0x1c
#define DMAR_PMEN_REG			0x64
#define DMAR_PLMBASE_REG		0x68
#define DMAR_PLMLIMIT_REG		0x6c
#define DMAR_PHMBASE_REG		0x70
#define DMAR_PHMLIMIT_REG		0x78

#define DMAR_CAP_PLMR			(1ULL << 5)
#define DMAR_CAP_PHMR			(1ULL << 6)

#define DMAR_GCMD_TE			(1U << 31)
#define DMAR_GSTS_TES			(1U << 31)
#define DMAR_GSTS_IRES			(1U << 25)

#define DMAR_PMEN_EPM			(1U << 31)
#define DMAR_PMEN_PRS			(1U << 0)

#define VTD_MAX_UNITS			8


struct vtd_unit {
	area_id						area;
	addr_t						registers;
	uint64						capabilities;
};


static acpi_module_info* sAcpi;
static struct vtd_unit sUnits[VTD_MAX_UNITS];
static uint32 sUnitCount;

int32 api_version = B_CUR_DRIVER_API_VERSION;


static inline uint32
vtd_read32(struct vtd_unit* unit, uint32 offset)
{
	return *(volatile uint32*)(unit->registers + offset);
}


static inline void
vtd_write32(struct vtd_unit* unit, uint32 offset, uint32 value)
{
	*(volatile uint32*)(unit->registers + offset) = value;
}


static inline uint64
vtd_read64(struct vtd_unit* unit, uint32 offset)
{
	return *(volatile uint64*)(unit->registers + offset);
}


/*!	Lifts the pre-boot DMA protection of one remapping unit.

	While the protected memory regions are enabled, devices cannot reach the
	memory inside them, which every driver allocating DMA buffers the ordinary
	way is bound to run into.
*/
static void
vtd_disable_protected_memory(struct vtd_unit* unit)
{
	if ((unit->capabilities & (DMAR_CAP_PLMR | DMAR_CAP_PHMR)) == 0)
		return;

	uint32 protection = vtd_read32(unit, DMAR_PMEN_REG);
	if ((protection & (DMAR_PMEN_EPM | DMAR_PMEN_PRS)) == 0)
		return;

	TRACE("protected memory active (low 0x%08" B_PRIx32 "-0x%08" B_PRIx32
		", high 0x%08" B_PRIx32 "-0x%08" B_PRIx32 "), disabling it\n",
		vtd_read32(unit, DMAR_PLMBASE_REG),
		vtd_read32(unit, DMAR_PLMLIMIT_REG),
		vtd_read32(unit, DMAR_PHMBASE_REG),
		vtd_read32(unit, DMAR_PHMLIMIT_REG));

	vtd_write32(unit, DMAR_PMEN_REG, protection & ~DMAR_PMEN_EPM);

	// The regions are only really gone once the status bit follows.
	for (int32 tries = 0; tries < 1000; tries++) {
		if ((vtd_read32(unit, DMAR_PMEN_REG) & DMAR_PMEN_PRS) == 0) {
			TRACE("protected memory disabled\n");
			return;
		}
		snooze(1000);
	}

	dprintf("vtd: protected memory regions stayed enabled\n");
}


static status_t
vtd_add_unit(uint64 address)
{
	if (sUnitCount >= VTD_MAX_UNITS)
		return B_NO_MEMORY;

	struct vtd_unit* unit = &sUnits[sUnitCount];

	void* registers;
	unit->area = map_physical_memory("vtd registers", address, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&registers);
	if (unit->area < 0) {
		dprintf("vtd: failed to map the registers at %#" B_PRIx64 ": %s\n",
			address, strerror(unit->area));
		return unit->area;
	}

	unit->registers = (addr_t)registers;
	unit->capabilities = vtd_read64(unit, DMAR_CAP_REG);

	uint32 version = vtd_read32(unit, DMAR_VER_REG);
	uint32 status = vtd_read32(unit, DMAR_GSTS_REG);

	TRACE("unit %" B_PRIu32 " at %#" B_PRIx64 ": version %" B_PRIu32 ".%"
		B_PRIu32 ", cap %#" B_PRIx64 ", ecap %#" B_PRIx64 ", status %#"
		B_PRIx32 "%s%s\n", sUnitCount, address, (version >> 4) & 0xf,
		version & 0xf, unit->capabilities, vtd_read64(unit, DMAR_ECAP_REG),
		status, (status & DMAR_GSTS_TES) != 0 ? ", translation enabled" : "",
		(status & DMAR_GSTS_IRES) != 0 ? ", interrupt remapping enabled" : "");

	sUnitCount++;

	vtd_disable_protected_memory(unit);

	return B_OK;
}


static status_t
vtd_scan_dmar()
{
	dmar_table* table;
	status_t status = sAcpi->get_table("DMAR", 1, (void**)&table);
	if (status != B_OK) {
		TRACE("no DMAR table, the firmware exposes no remapping hardware\n");
		return B_OK;
	}

	TRACE("DMAR: host address width %u bits, flags %#x\n",
		table->host_address_width + 1, table->flags);

	addr_t offset = sizeof(dmar_table);
	while (offset + sizeof(dmar_entry) <= table->header.length) {
		dmar_entry* entry = (dmar_entry*)((addr_t)table + offset);
		if (entry->length < sizeof(dmar_entry)
			|| offset + entry->length > table->header.length) {
			break;
		}

		if (entry->type == DMAR_TYPE_DRHD
			&& entry->length >= sizeof(dmar_drhd)) {
			vtd_add_unit(((dmar_drhd*)entry)->address);
		}

		offset += entry->length;
	}

	return B_OK;
}


//	#pragma mark - driver hooks


status_t
init_hardware(void)
{
	return B_OK;
}


status_t
init_driver(void)
{
	if (get_module(B_ACPI_MODULE_NAME, (module_info**)&sAcpi) != B_OK)
		return B_ERROR;

	status_t status = vtd_scan_dmar();
	if (status != B_OK || sUnitCount == 0) {
		put_module(B_ACPI_MODULE_NAME);
		sAcpi = NULL;
		return B_OK;
	}

	return B_OK;
}


void
uninit_driver(void)
{
	for (uint32 i = 0; i < sUnitCount; i++)
		delete_area(sUnits[i].area);

	sUnitCount = 0;

	if (sAcpi != NULL) {
		put_module(B_ACPI_MODULE_NAME);
		sAcpi = NULL;
	}
}


const char**
publish_devices(void)
{
	static const char* devices[] = { NULL };
	return devices;
}


device_hooks*
find_device(const char* name)
{
	return NULL;
}
