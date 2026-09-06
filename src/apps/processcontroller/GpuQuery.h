#ifndef GPU_QUERY_H
#define GPU_QUERY_H

#include <SupportDefs.h>
#include <Locker.h>

struct GpuClientInfo {
	team_id		team;
	uint32_t	contexts;
	uint64_t	ticks;
	double		usage; // 0.0 to 1.0
};

class GpuQuery {
public:
	GpuQuery();
	~GpuQuery();

	bool		IsAvailable() const;
	void		Open();
	void		Close();
	void		Query();

	double		GetTeamGpuUsage(team_id team);
	double		GetTotalGpuUsage();

	uint32_t	GetClientCount();
	bool		GetClientInfo(uint32_t index, GpuClientInfo& info);
	uint32_t	GetFrequencyMhz();
	const char*	GetDeviceName();

private:
	void		_OpenLocked();

	int			fFd;
	BLocker		fLock;
	uint64_t	fLastTotalTicks;
	struct TeamTicks {
		team_id		team;
		uint64_t	ticks;
	};
	TeamTicks	fLastClients[16];
	uint32_t	fLastClientCount;
	uint64_t	fLastTimestamp;
	double		fTotalUsage;
	uint32_t	fFrequencyMhz;
	char		fDeviceName[64];

	GpuClientInfo	fClients[16];
	uint32_t		fClientCount;
};

extern GpuQuery* gGpuQuery;

#endif // GPU_QUERY_H
