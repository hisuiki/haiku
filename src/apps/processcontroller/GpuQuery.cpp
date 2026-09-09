#include "GpuQuery.h"

#include <Directory.h>
#include <Entry.h>
#include <Path.h>
#include <Drivers.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <OS.h>

GpuQuery* gGpuQuery = NULL;

namespace {

const uint32 kABIVersion = 8;
const uint32 kGpuTimestampHz = 12000000;
const uint32 kMaxActivityClients = 16;

enum {
	kGetInfo = B_DEVICE_OP_CODES_END + 0x4900 + 0,
	kReadRegister = B_DEVICE_OP_CODES_END + 0x4900 + 8,
	kGpuActivity = B_DEVICE_OP_CODES_END + 0x4900 + 20
};

struct Header {
	uint32_t version;
	uint32_t size;
};

struct ActivityClient {
	int32_t  team;
	uint32_t contexts;
	uint64_t ticks;
};

struct GpuActivity {
	Header header;
	uint32_t timestampHz;
	uint32_t count;
	uint64_t totalTicks;
	uint64_t reserved;
	ActivityClient clients[kMaxActivityClients];
};

struct ReadRegister {
	Header   header;
	uint32_t offset;
	uint32_t value;
};

struct DeviceInfo {
	Header   header;
	uint64_t capabilities;
	uint64_t maxBufferSize;
	uint64_t graphicsAddressBase;
	uint64_t graphicsAddressSize;
	uint16_t vendor;
	uint16_t device;
	uint8_t  revision;
	uint8_t  bus;
	uint8_t  slot;
	uint8_t  function;
	uint32_t graphicsVersion;
	uint32_t reserved;
};

} // anonymous namespace


GpuQuery::GpuQuery()
	: fFd(-1),
	  fLock("GpuQueryLock"),
	  fLastTotalTicks(0),
	  fLastClientCount(0),
	  fLastTimestamp(0),
	  fTotalUsage(0.0),
	  fFrequencyMhz(0),
	  fClientCount(0)
{
	strlcpy(fDeviceName, "Intel Graphics", sizeof(fDeviceName));
	memset(fLastClients, 0, sizeof(fLastClients));
	memset(fClients, 0, sizeof(fClients));
}

GpuQuery::~GpuQuery()
{
	Close();
}

bool
GpuQuery::IsAvailable() const
{
	return fFd >= 0;
}

void
GpuQuery::Open()
{
	fLock.Lock();
	_OpenLocked();
	fLock.Unlock();
}

void
GpuQuery::_OpenLocked()
{
	if (fFd >= 0)
		return;

	BDirectory dir("/dev/graphics");
	if (dir.InitCheck() != B_OK)
		return;

	BEntry entry;
	while (dir.GetNextEntry(&entry) == B_OK) {
		char name[B_FILE_NAME_LENGTH];
		entry.GetName(name);
		if (strncmp(name, "intel_extreme_", 14) == 0) {
			BPath path;
			entry.GetPath(&path);
			fFd = open(path.Path(), O_RDWR);
			if (fFd >= 0) {
				GpuActivity activity = {};
				activity.header.version = kABIVersion;
				activity.header.size = sizeof(GpuActivity);
				if (ioctl(fFd, kGpuActivity, &activity, sizeof(GpuActivity)) >= 0) {
					DeviceInfo info = {};
					info.header.version = kABIVersion;
					info.header.size = sizeof(DeviceInfo);
					if (ioctl(fFd, kGetInfo, &info, sizeof(DeviceInfo)) >= 0) {
						if (info.device == 0x191b)
							strlcpy(fDeviceName, "Intel HD Graphics 530", sizeof(fDeviceName));
						else if (info.graphicsVersion >= 9)
							snprintf(fDeviceName, sizeof(fDeviceName), "Intel Gen9 (%04x)", info.device);
						else if (info.graphicsVersion > 0)
							snprintf(fDeviceName, sizeof(fDeviceName), "Intel Gen%" B_PRIu32, info.graphicsVersion);
						else
							strlcpy(fDeviceName, "Intel Graphics", sizeof(fDeviceName));
					}
					return;
				}
				close(fFd);
				fFd = -1;
			}
		}
	}
}

void
GpuQuery::Close()
{
	fLock.Lock();
	if (fFd >= 0) {
		close(fFd);
		fFd = -1;
	}
	fLock.Unlock();
}

void
GpuQuery::Query()
{
	fLock.Lock();
	if (fFd < 0) {
		_OpenLocked();
		if (fFd < 0) {
			fLock.Unlock();
			return;
		}
	}

	uint64_t now = system_time();
	if (fLastTimestamp > 0 && (now - fLastTimestamp) < 50000) {
		fLock.Unlock();
		return;
	}

	GpuActivity activity = {};
	activity.header.version = kABIVersion;
	activity.header.size = sizeof(GpuActivity);
	if (ioctl(fFd, kGpuActivity, &activity, sizeof(GpuActivity)) < 0) {
		fLock.Unlock();
		return;
	}

	ReadRegister reg = {};
	reg.header.version = kABIVersion;
	reg.header.size = sizeof(ReadRegister);
	reg.offset = 0xa01c;
	if (ioctl(fFd, kReadRegister, &reg, sizeof(ReadRegister)) >= 0) {
		fFrequencyMhz = ((reg.value >> 23) & 0x1ff) * 50 / 3;
	}

	GpuClientInfo current[kMaxActivityClients] = {};
	uint32_t currentCount = 0;
	for (uint32_t i = 0; i < activity.count && i < kMaxActivityClients; i++) {
		uint32_t index = currentCount;
		for (uint32_t j = 0; j < currentCount; j++) {
			if (current[j].team == activity.clients[i].team) {
				index = j;
				break;
			}
		}
		if (index == currentCount) {
			current[index].team = activity.clients[i].team;
			currentCount++;
		}
		current[index].contexts += activity.clients[i].contexts;
		current[index].ticks += activity.clients[i].ticks;
	}

	now = system_time();
	fClientCount = 0;
	if (fLastTimestamp > 0 && now > fLastTimestamp) {
		double dt = (now - fLastTimestamp) / 1000000.0;
		double hz = activity.timestampHz > 0 ? activity.timestampHz : kGpuTimestampHz;

		uint64_t deltaTotal = 0;
		if (activity.totalTicks >= fLastTotalTicks)
			deltaTotal = activity.totalTicks - fLastTotalTicks;
		fTotalUsage = (double)deltaTotal / (hz * dt);
		if (fTotalUsage > 1.0) fTotalUsage = 1.0;
		if (fTotalUsage < 0.0) fTotalUsage = 0.0;

		for (uint32_t i = 0; i < currentCount; i++) {
			uint64_t delta = 0;
			for (uint32_t j = 0; j < fLastClientCount; j++) {
				if (fLastClients[j].team == current[i].team) {
					if (current[i].ticks >= fLastClients[j].ticks)
						delta = current[i].ticks - fLastClients[j].ticks;
					break;
				}
			}

			double usage = (double)delta / (hz * dt);
			if (usage > 1.0) usage = 1.0;
			if (usage < 0.0) usage = 0.0;

			fClients[fClientCount] = current[i];
			fClients[fClientCount].usage = usage;
			fClientCount++;
		}
	}

	fLastTotalTicks = activity.totalTicks;
	fLastClientCount = currentCount;
	for (uint32_t i = 0; i < currentCount; i++) {
		fLastClients[i].team = current[i].team;
		fLastClients[i].ticks = current[i].ticks;
	}
	fLastTimestamp = now;
	fLock.Unlock();
}

double
GpuQuery::GetTeamGpuUsage(team_id team)
{
	fLock.Lock();
	double usage = 0.0;
	for (uint32_t i = 0; i < fClientCount; i++) {
		if (fClients[i].team == team) {
			usage = fClients[i].usage;
			break;
		}
	}
	fLock.Unlock();
	return usage;
}

double
GpuQuery::GetTotalGpuUsage()
{
	fLock.Lock();
	double usage = fTotalUsage;
	fLock.Unlock();
	return usage;
}

uint32_t
GpuQuery::GetClientCount()
{
	fLock.Lock();
	uint32_t count = fClientCount;
	fLock.Unlock();
	return count;
}

bool
GpuQuery::GetClientInfo(uint32_t index, GpuClientInfo& info)
{
	fLock.Lock();
	if (index >= fClientCount) {
		fLock.Unlock();
		return false;
	}
	info = fClients[index];
	fLock.Unlock();
	return true;
}

uint32_t
GpuQuery::GetFrequencyMhz()
{
	fLock.Lock();
	uint32_t freq = fFrequencyMhz;
	fLock.Unlock();
	return freq;
}

const char*
GpuQuery::GetDeviceName()
{
	return fDeviceName;
}
