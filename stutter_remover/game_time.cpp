#include "main.h"
#include "game_time.h"
#include <mmsystem.h>
#include "settings.h"
#include "locks.h"

static UInt32 current_time;
static UInt32 bias_tgt;
static UInt32 bias_gtc;
static CRITICAL_SECTION timelock;

static bool reset = true;

void update_GetTickCount() {//called at the start of each frame
	reset = true;
}

UInt32 GetTickCount_replacement_unsynced() {
	using namespace Settings::GetTickCount;
	if (bForceResolution) {
		UInt32 t = timeGetTime() - bias_tgt;
		if (!bPreserveHighFreqComponents) return t;
		UInt32 t2 = GetTickCount() - bias_gtc;
		int diff = t - t2;
		if (diff >= 0 && diff < 15) return t;
		if (diff == 16) return t2 + 15;
		EnterCriticalSection(&timelock);
		message("GetTickCount - bPreserveHighFreqComponents - diff %d", diff);
		if (diff < 0) {bias_tgt += diff; t -= diff;}
		else {bias_tgt += diff - 16; t -= diff - 16;}
		LeaveCriticalSection(&timelock);
		return t;
	}
	else return GetTickCount() - bias_gtc;
}
UInt32 GetTickCount_replacement_synced() {//bForceSync is true
	using namespace Settings::GetTickCount;
	UInt32 t = GetTickCount_replacement_unsynced();

	if (reset) {
		PerThread *pt = PerThread::get_PT();
		if (pt->thread_number == 1) {
			current_time = t;
			reset = false;
			return t;
		}
	}
	SInt32 delta = t-current_time;
	if (delta < 0) {
		message("GetTickCount_replacement_synced - negative delta %d", delta);
		return current_time;
	}
	if (delta < iSyncLimitMilliseconds) return current_time;
	else return t - iSyncLimitMilliseconds;//or should it be just "return t;" ?
}

void hook_GetTickCount() {
	using namespace Settings::GetTickCount;
	message("hook_GetTickCount bPreserveDC_Bias=%d Sync=%dms bForceResolution=%d bPreserveHighFreqComponents=%d bPreserveHighFreqComponents=%d", 
		bPreserveDC_Bias, bForceSync ? iSyncLimitMilliseconds : 0, bForceResolution, bPreserveHighFreqComponents, bPreserveHighFreqComponents);
	if (bPreserveDC_Bias) {
		bias_gtc = 0;
		UInt32 t1 = GetTickCount(), t2;
		while (t1 == (t2 = GetTickCount())) ;
		bias_tgt = timeGetTime() - t2;
	}
	else {
		bias_gtc = PerThread::get_PT()->internal_rng_hq.raw32();
		bias_tgt = timeGetTime() - (GetTickCount() - bias_gtc);
	}
	SafeWrite32( UInt32(Hook_GetTickCount_Indirect), 
		UInt32(bForceSync ? &GetTickCount_replacement_synced : &GetTickCount_replacement_unsynced)
	);
}
