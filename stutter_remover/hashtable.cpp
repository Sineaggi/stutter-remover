
#include <set>
#include <map>
#include <list>
#include <string>
#include <vector>
#include "main.h"
#include "locks.h"
#include "hashtable.h"
#include "memory.h"
#include "struct_text.h"

#include "settings.h"

struct Hashtable {
	struct Entry
	{
		Entry	* next;
		UInt32	key;//or a string pointer, or something
		void	* data;
	};

	void **vtable;  //00
	/*
		vtable offset
		n/a		-4		RTTI of some kind
		n/a		vt_0	destructor (often not called through the vtable)
		int		vt_4	hash function (returns bucket index)
		bool	vt_8	(Key k1, Key k2)						{return k1 == k2;}
		?		vt_C	(Entry*, int bucketindex, void *data)	at least sometimes called right before insertion with the data to be inserted
		?		vt_10	(Entry*)								possibly called as an Entry is removed/deleted ?
		Entry *	vt_14	()			allocate node
		Entry *	vt_18	(Entry*)	deallocate node ; sometimes it deletes Entrys passed to it, other times it does something dumb involving DecodePointer
	*/
	UInt32	m_numBuckets;//04
	Entry	** m_buckets;//08
	UInt32	m_numItems;  //0C
	//char/bool unkown;//10

/*	UInt32 get_valid_size() {
		UInt32 rv = m_numBuckets;
		if (rv != UInt32(-1)) return rv;
		int tries = 0;
		while (true) {
			Sleep( 1 + (Settings::Hashtables::iHashtableResizeDelay>>2) );
			MemoryBarrier();
			rv = m_numBuckets;
			if (rv != UInt32(-1)) return rv;
			if (tries++ > 4) {
				message("Hashtable::get_valid_size() - taking too long (%d tries)", tries);
			}
		}
	}*/
};



bool is_hashtable_on_stack( Hashtable *ht ) {
	return (UInt32(ht) - UInt32(&ht)) < (1<<15);
}
namespace HT_Perf1 {
	static CRITICAL_SECTION htcs;
	enum {MEMSET_CALLER_TABLE_SIZE = 1<<16};
	struct PerMemset {
		void *memory;
		UInt32 caller;
	};
	volatile PerMemset *memset_callers;
	static UInt32 memset_address_hash(void *address) {
		UInt32 index = (UInt32)address;
		index += index << 3;
		index ^= index >> 16;
		index *= 0x9C145B25;
		index ^= index >> 16;
		index *= 0xA749E665;
		index ^= index >> 16;
		return index;
	}
	static void *__cdecl fake_memset(void *dest, int value, size_t size) {
		volatile PerMemset &pm = memset_callers[memset_address_hash(dest) & (MEMSET_CALLER_TABLE_SIZE-1)];
		pm.memory = dest;
		pm.caller = (UInt32)((&dest)[-1]);
		return ::memset(dest, value, size);
	}
	static UInt32 get_memsetter(void *memory) {
		volatile PerMemset &pm = memset_callers[memset_address_hash(memory) & (MEMSET_CALLER_TABLE_SIZE-1)];
		if (pm.memory != memory) return 0;
		return pm.caller;
	}

	struct DeletedHashtableData {
		//when a hashtable is deleted, it is associated with one of these on a per (size,memsetter) basis
		UInt32 accesses;
		UInt64 wastage;
		UInt32 memsetter;
		UInt32 size;
		UInt32 peak;
		UInt32 death_count;
		std::set<UInt32> vtables;
		DeletedHashtableData() : accesses(0), wastage(0), memsetter(0), size(0), peak(0), death_count(0) {}
	};
	std::map<std::pair<UInt32,UInt32>, DeletedHashtableData> dhtd_map;
	enum {MAX_HASHTABLES = 1<<15};
	struct HashtableFakeVTable;
	extern HashtableFakeVTable *hashtable_marked_vtable_space;
	struct HashtableFakeVTable {
		void *rtti;			//0 maybe?
		void *destructor;	//1
		void *others[6];	//2-7
		void **old_vtable;	//8
		UInt32 address;		//9
		UInt64 wastage;		//10-11
		UInt32 accesses;	//12
		UInt32 size;		//13
		UInt32 peak;		//14 - largest # of items seen in hashtables
		UInt32 memsetter;	//15
		//UInt32 padding[1];
		volatile bool allocate(Hashtable *ht);
		static UInt32 destruction_helper(Hashtable *ht, HashtableFakeVTable *fv) {
			//int slot = (UInt32(fv) - UInt32(hashtable_marked_vtable_space)) / sizeof(HashtableFakeVTable);
			UInt32 odtor = UInt32(fv->old_vtable[0]);

			::EnterCriticalSection(&htcs);
			if (fv->destructor) {
				DeletedHashtableData &dhtd = dhtd_map[std::pair<UInt32,UInt32>(fv->memsetter, fv->size)];
				dhtd.accesses += fv->accesses;
				dhtd.wastage += fv->wastage;
				if (dhtd.peak < fv->peak) dhtd.peak = fv->peak;
				if (!dhtd.memsetter) dhtd.memsetter = fv->memsetter;
				if (!dhtd.size) dhtd.size = fv->size;
				dhtd.death_count++;
				dhtd.vtables.insert(UInt32(fv->old_vtable));
				fv->destructor = nullptr;
			}
			::LeaveCriticalSection(&htcs);
			return odtor;
		}
		UInt32 _destruction_helper() {
			Hashtable *ht = (Hashtable*)this;
			HashtableFakeVTable *fv = (HashtableFakeVTable*)(UInt32(ht->vtable) - 4);
			return destruction_helper(ht, fv);
		}
	};
	static __declspec(naked) void fake_destructor() {
		__asm {
			push ecx
			call HashtableFakeVTable::_destruction_helper
			pop ecx
			jmp eax
		}
	}
	volatile bool HashtableFakeVTable::allocate(Hashtable *ht) {
		if (InterlockedCompareExchangePointer(&destructor, &fake_destructor, nullptr)) {
			return false;
		}
		void **rvt = ht->vtable;
		rtti = rvt[-1];
		old_vtable = ht->vtable;
		for (int i = 0; i < 6; i++) others[i] = rvt[i+1];

		address = UInt32(ht);
		accesses = 0;
		wastage = 0;
		size = ht->m_numBuckets;
		peak = ht->m_numItems;
		memsetter = get_memsetter(ht->m_buckets);
		return true;
	}
	int kill_undead_hashtables() {//returns number slain
		int slain_undead = 0;
		EnterCriticalSection(&htcs);
		for (int i = 0; i < MAX_HASHTABLES; i++) {
			HashtableFakeVTable &d = hashtable_marked_vtable_space[i];
			void *dtor = (void*)d.destructor;
			if (!dtor) continue;
			Hashtable *ht = (Hashtable*)d.address;
			if (ht->vtable == &d.destructor) continue;
			if (HashtableFakeVTable::destruction_helper(nullptr, &d) != 0)
				slain_undead++;
		}
		LeaveCriticalSection(&htcs);
		return slain_undead;
	}
	HashtableFakeVTable *hashtable_marked_vtable_space = nullptr;


	UInt32 last_print_time;
	void print_summary();
	void _notify(Hashtable *ht, char type) {
		HashtableFakeVTable *fv;
		if (UInt32(ht->vtable) - UInt32(hashtable_marked_vtable_space) < MAX_HASHTABLES * sizeof(HashtableFakeVTable)) {
			fv = (HashtableFakeVTable*)(ht->vtable-1);//already marked
		}
		else {
			if (!hashtable_marked_vtable_space) {
				error("hashtable stuff not properly initialized");
			}
			PerThread *pt = PerThread::get_PT();
			for (int tries = 0; true; tries++) {
				if (tries >= 10) {//how bloody full is the FVT space?
					message("HT_Perf1::_notify - attempt #%d at allocating an FVT slot failed (getting full?)", tries);
					if (tries == 10) {
						message("   maybe full of undead hashtables?  trying to kill undead hashtables now");
						int slain = kill_undead_hashtables();
						message("   %d undead hashtables were killed", slain);
					}
					if (tries > 99) error("HT_Perf1::_notify - FVT slots too full, aborting");
				}
				int index = pt->internal_rng.raw16() & (MAX_HASHTABLES-1);
				if (hashtable_marked_vtable_space[index].allocate(ht)) {
					fv = &hashtable_marked_vtable_space[index];
					ht->vtable = (void**)&fv->destructor;
					break;
				}
			}
		}

		fv->accesses++;
		if (ht->m_numItems > ht->m_numBuckets) fv->wastage += (ht->m_numItems << 4) / ht->m_numBuckets - 16;
		if (ht->m_numItems > fv->peak) fv->peak = ht->m_numItems;
		UInt32 time = get_time_ms();
		if (time - last_print_time > 13000) {
			last_print_time = time;
			print_summary();
		}
	}
	void notify_int( Hashtable *ht ) {
		_notify(ht, 'i');
	}
	void notify_string( Hashtable *ht ) {
		_notify(ht, 's');
	}
	void print_summary() {
		EnterCriticalSection(&htcs);
		message("HT_Perf1::print_summary (time=%ds)", get_time_ms()/1000);
		if (true) {
			message("  alive hashtables");
			enum {MAX_ALIVE = 50};
			int undead_hashtables = 0;
			std::map<double, HashtableFakeVTable*> sorted;
			for (int i = 0; i < MAX_HASHTABLES; i++) {
				HashtableFakeVTable &d = hashtable_marked_vtable_space[i];
				if (!d.destructor) continue;
				Hashtable *ht = (Hashtable*)d.address;
				if (ht->vtable != &d.destructor) {
					undead_hashtables++;
					continue;
				}
				double weight = d.accesses + d.wastage/16.0;
				sorted.insert(std::pair<double, HashtableFakeVTable*>(weight, &d));
			}
			std::map<double, HashtableFakeVTable*>::reverse_iterator it2 = sorted.rbegin();
			if (sorted.size() > MAX_ALIVE) message("  (%d entries, capped at %d)", sorted.size(), MAX_ALIVE);
			if (undead_hashtables) {
				message("  (%d undead hashtables found... trying to destroy them now)", undead_hashtables);
				int slain_undead = kill_undead_hashtables();
				message("  (%d of %d undead hashtables slain)", slain_undead, undead_hashtables);
			}
			int n = 0;
			for (;it2 != sorted.rend() && n < MAX_ALIVE; it2++,n++) {
				HashtableFakeVTable &d = *it2->second;
				if (!d.destructor) continue;
				char buffy[800];
				char *p = buffy;
				// FIXME
				p += sprintf_s(p, 800, "   %9d%9.0f  (%4.1f)    %6d/%6d     ms:0x%08X  addr:0x%08X  vt:0x%08X", 
					d.accesses, double(d.wastage>>4), double(d.wastage)/d.accesses / 16.0, d.peak, d.size, d.memsetter, d.address, d.old_vtable
				);
				message(buffy);
			}
		}
		if (true) {
			message("  dead hashtables");
			enum {MAX_DEAD = 50};
			std::map<double, DeletedHashtableData*> sorted;
			//std::map<std::pair<UInt32,UInt32>, DeletedHashtableData> dhtd_map;
			std::map<std::pair<UInt32,UInt32>, DeletedHashtableData>::iterator it1 = dhtd_map.begin();
			for (;it1 != dhtd_map.end(); it1++) {
				double weight = it1->second.accesses + it1->second.wastage/16.0 + it1->second.death_count * 0.5 * it1->second.size;
				sorted.insert(std::pair<double,DeletedHashtableData*>(weight, &it1->second));
			}
			std::map<double, DeletedHashtableData*>::reverse_iterator it2 = sorted.rbegin();
			if (sorted.size() > MAX_DEAD) message("  (%d entries, capped at %d)", sorted.size(), MAX_DEAD);
			int n = 0;
			for (;it2 != sorted.rend() && n < MAX_DEAD; it2++,n++) {
				DeletedHashtableData &d = *it2->second;
				char buffy[800];
				char *p = buffy;
				// FIXME
				p += sprintf_s(p, 800, "   %9d%9.0f  (%4.1f)  %6d/%6d ms:0x%08X  %6d (%4.1f)", 
					d.accesses, double(d.wastage>>4), double(d.wastage)/d.accesses/16, d.peak, d.size, d.memsetter, d.death_count, 
					d.size * d.death_count * 0.5 / (d.wastage / 16.0 +  d.accesses)
				);
				// FIXME
				for (std::set<UInt32>::iterator it = d.vtables.begin(); it != d.vtables.end(); it++) if (*it) p += sprintf_s(p, 800, " vt:0x%08x", *it);
				message(buffy);
			}
		}
		message("end HT_Perf1::print_summary");
		LeaveCriticalSection(&htcs);
	}
	void init() {
		if (Settings::Hashtables::bEnableMessages) message("HT_Perf1::init");
		InitializeCriticalSectionAndSpinCount(&htcs,1000);
		last_print_time = get_time_ms() - 15000;
		if (sizeof(HashtableFakeVTable) != 64) message("HashtableFakeVTable size not well-aligned (%d)", sizeof(HashtableFakeVTable));
		hashtable_marked_vtable_space = new HashtableFakeVTable[MAX_HASHTABLES];
		memset(hashtable_marked_vtable_space, 0, MAX_HASHTABLES * sizeof(HashtableFakeVTable));
		memset_callers = new PerMemset[MEMSET_CALLER_TABLE_SIZE];
		memset((void*)memset_callers, 0, MEMSET_CALLER_TABLE_SIZE * sizeof(PerMemset));
		if (Hook_memset1) {
			if (Settings::Hashtables::bEnableMessages && Settings::Hashtables::bEnableExtraMessages) message("hooking memset");
			WriteRelJump(Hook_memset1, (UInt32)&fake_memset);
			if (Hook_memset2) WriteRelJump(Hook_memset2, (UInt32)&fake_memset);
		}
	}
};

#define HT_Perf HT_Perf1

int    __fastcall hashint_normal ( Hashtable *ht, int dummy, UInt32 key ) {
	return key % ht->m_numBuckets;
}
int __fastcall hashstring_normal ( Hashtable *ht, int dummy, const char *key ) {
	UInt32 hash = 0;
	for (signed char c; c = *key; key++) {
		hash = (hash << 5) + hash + c;
	}
	return hash % ht->m_numBuckets;
}
int    __fastcall hashint_log ( Hashtable *ht, int dummy, UInt32 key ) {
	HT_Perf::notify_int(ht);
	return key % ht->m_numBuckets;
}
int __fastcall hashstring_log ( Hashtable *ht, int dummy, const char *key ) {
	HT_Perf::notify_string(ht);
	UInt32 hash = 0;
	for (signed char c; c = *key; key++) {
		hash = (hash << 5) + hash + c;
	}
	return hash % ht->m_numBuckets;
}


int hashfunc_normal_int ( UInt32 key, UInt32 buckets ) {
	return key % buckets;
}
int hashfunc_normal_string ( UInt32 key_, UInt32 buckets ) {
	char *key = (char*)key_;
	UInt32 hash = 0;
	for (signed char c; c = *key; key++) {
		hash = (hash << 5) + hash + c;
	}
	return hash % buckets;
}
int hashfunc_alt_int ( UInt32 key, UInt32 buckets ) {
	__asm {
		mov eax, key
		mov edx, 0xB234DD0B
		mul edx
		shl edx, 15
		xor edx, key
		add eax, edx
		mul buckets
		mov eax, edx
	}
}
int hashfunc_alt_string ( UInt32 key_, UInt32 buckets ) {
	signed char *key = (signed char*)key_;
	UInt32 hash = *key;
	if (hash) {
		hash ^= hash << 8;
		hash ^= hash >> 3;
		key++;
		for (signed char c; c = *key; key++) {
			UInt32 rot = ((hash << 19) | (hash >> 13));
			hash += c + 1;
			hash ^= rot;
		}
	}
	hash ^= (hash << 4);
	hash += (hash << 24);
	__asm {
		mov eax, hash
		mul buckets
		mov eax, edx
	}
}

int    __fastcall hashint_alternate ( Hashtable *ht, int dummy, UInt32 key ) {
	return hashfunc_alt_int(key, ht->m_numBuckets);
}
int __fastcall hashstring_alternate ( Hashtable *ht, int dummy, const char *key ) {
	return hashfunc_alt_string(UInt32(key), ht->m_numBuckets);
}

bool is_prime(int value) {
	static UInt32 small_primes = 
		(1 << 2) + (1 << 3) + (1 << 5) + (1 << 7) + (1 <<11) + (1 <<13) + 
		(1 <<17) + (1 <<19) + (1 <<23) + (1 <<29);
	if (value <= 0) error("invalid parameter to is_prime (%08X)", value);
	if (value < 32) return ((small_primes >> value) & 1) ? true : false;
	if (!(value % 3)) return false;
	if (!(value % 5)) return false;
	if (!(value % 7)) return false;
	if (!(value % 11)) return false;
	else if (!(value % 13)) return false;
	else if (value < 17 * 17) return true;
	else if (!(value % 17)) return false;
	else if (!(value % 19)) return false;
	else if (!(value % 23)) return false;
	else if (value < 29 * 29) return true;
	else if (!(value % 29)) return false;
	else if (!(value % 31)) return false;
	else if (!(value % 37)) return false;
	else if (value < 41 * 41) return true;
	else if (!(value % 41)) return false;
	else if (!(value % 43)) return false;
	else if (!(value % 47)) return false;
	else if (!(value % 53)) return false;
	//don't really care much about large divisors for this purpose, ignore them
	return true;
}
int find_next_prime ( int size ) {
	size |= 1;
	if (size > (3<<17)) return size;
	while (!is_prime(size)) size += 2;
	return size;
}

void initialize_hashtable_hooks() {
	using namespace Settings::Hashtables;
	if (bEnableProfiling && (Hook_Hashtable_IntToIndex1 || Hook_Hashtable_StringToIndex1)) {
		message("static-size hashtables with profiling");
		if (Hook_Hashtable_IntToIndex1)    WriteRelJump(Hook_Hashtable_IntToIndex1, UInt32(hashint_log));
		if (Hook_Hashtable_IntToIndex2)    WriteRelJump(Hook_Hashtable_IntToIndex2, UInt32(hashint_log));
		if (Hook_Hashtable_IntToIndex3)    WriteRelJump(Hook_Hashtable_IntToIndex3, UInt32(hashint_log));
		if (Hook_Hashtable_IntToIndex4)    WriteRelJump(Hook_Hashtable_IntToIndex4, UInt32(hashint_log));
		if (Hook_Hashtable_StringToIndex1) WriteRelJump(Hook_Hashtable_StringToIndex1, UInt32(hashstring_log));
		if (Hook_Hashtable_StringToIndex2) WriteRelJump(Hook_Hashtable_StringToIndex2, UInt32(hashstring_log));
		if (Hook_Hashtable_StringToIndex3) WriteRelJump(Hook_Hashtable_StringToIndex3, UInt32(hashstring_log));
		if (Hook_Hashtable_StringToIndex4) WriteRelJump(Hook_Hashtable_StringToIndex4, UInt32(hashstring_log));

		HT_Perf::init();
	}
	if (bUseOverrides) {
		int num_hashtable_overrides = 0;
		if (bEnableMessages) message("using hashtable size overrides");
		TextSection *ts = config->get_section("OverrideList");
		if (ts) for (ts = ts->get_first_section(); ts; ts = ts->get_next_section()) {
			if (ts->get_name() == "Hashtable") {
				TextSection *addr = ts->get_section("SizeAddress");
				TextSection *size = ts->get_section("NewSize");
				TextSection *old = ts->get_section("OldSize");
				TextSection *bits = ts->get_section("WordBits");
				if (!addr) {
					message("Hashtable override ignored - missing SizeAddress");
					continue;
				}
				if (!size) {
					message("Hashtable override ignored - missing NewSize");
					continue;
				}
				if (!old) {
					message("Hashtable override ignored - missing OldSize");
					continue;
				}
				UInt32 a, s, o, b;
				a = addr->get_int();
				s = size->get_int();
				o = old->get_int();
				b = bits ? bits->get_int() : 32;
				if (!a) { message("Hashtable override ignored: SizeAddress = 0?"); continue; }
				if (!s) { message("Hashtable override ignored: NewSize = 0?"); continue; }
				if (!o) { message("Hashtable override ignored: OldSize = 0?"); continue; }
#if defined NEW_VEGAS
				TextSection *version = ts->get_section("Version");
				const char *v = version ? version->get_c_string() : "";
				if (strcmp(v, Hook_target_s)) continue;
#endif
				if (b != 8 && b != 16 && b != 32) { message("Hashtable override ignored: WordBits not in {8,16,32}"); continue; }
				UInt32 max = (256 << (b - 8)) - 1;
				if (s > max) { message("Hashtable override ignored: NewSize invalid for that number of bits (%d > %d)", s, max); continue; }
				if (bEnableMessages && bEnableExtraMessages) message("Hashtable override - passed safe checks, attempting unsafe check");
				UInt32 actual_old;
				if (b == 32) actual_old = *((UInt32*)a);
				if (b == 16) actual_old = *((UInt16*)a);
				if (b == 8) actual_old = *((UInt8 *)a);
				if (o != actual_old) {
					message("Hashtable override ignored - failed unsafe check (%d != %d)", o, actual_old);
					continue;
				}
				else if (bEnableMessages && bEnableExtraMessages) message("Hashtable override - passed unsafe check (%d == %d)", o, actual_old);
				if (bEnableMessages) message("Hashtable size override: %08X : %d -> %d", a, o, s);
				if (false);
				else if (b == 32) SafeWrite32(a, s);
				else if (b == 16) SafeWrite16(a, s);
				else if (b == 8) SafeWrite8(a, s);
				num_hashtable_overrides++;
			}
			else if (ts->get_name() == "HashtableEarly") {
				TextSection *addr = ts->get_section("Address");
				TextSection *size = ts->get_section("NewSize");
				TextSection *old = ts->get_section("OldSize");
				if (!addr) {
					message("HashtableEarly override ignored - missing Address");
					continue;
				}
				if (!size) {
					message("HashtableEarly override ignored - missing NewSize");
					continue;
				}
				if (!old) {
					message("HashtableEarly override ignored - missing OldSize");
					continue;
				}
				UInt32 a, s, o;
				a = addr->get_int();
				s = size->get_int();
				o = old->get_int();
				if (!a) { message("HashtableEarly override ignored: Address = 0?"); continue; }
				if (!s) { message("HashtableEarly override ignored: NewSize = 0?"); continue; }
				if (!o) { message("HashtableEarly override ignored: OldSize = 0?"); continue; }
#if defined NEW_VEGAS
				TextSection *version = ts->get_section("Version");
				const char *v = version ? version->get_c_string() : "";
				if (strcmp(v, Hook_target_s)) continue;
#endif
				Hashtable *ht = (Hashtable *)a;
				UInt32 actual_old = ht->m_numBuckets;
				if (o != actual_old) {
					message("HashtableEarly override ignored - failed unsafe check (%d != %d)", o, actual_old);
					continue;
				}
				if (!ht->m_buckets) {
					message("HashtableEarly override ignored - not yet constructed? (%x)", a);
					continue;
				}
				if (ht->m_numItems) {
					message("HashtableEarly size override ignored - not empty (%X)", a);
					continue;
				}
				if (bEnableMessages) message("HashtableEarly size override: %08X : %d -> %d", a, o, s);
				ht->m_numBuckets = s; ht->m_buckets = (Hashtable::Entry**)malloc(4 * s); memset(ht->m_buckets, 0, 4 * s);
				num_hashtable_overrides++;
			}
			else if (ts->get_name() == "HashtableEarlyIndirect") {
				TextSection *addr = ts->get_section("Address");
				TextSection *size = ts->get_section("NewSize");
				TextSection *old = ts->get_section("OldSize");
				if (!addr) {
					message("HashtableEarlyIndirect override ignored - missing Address");
					continue;
				}
				if (!size) {
					message("HashtableEarlyIndirect override ignored - missing NewSize");
					continue;
				}
				if (!old) {
					message("HashtableEarlyIndirect override ignored - missing OldSize");
					continue;
				}
				UInt32 a, s, o;
				a = addr->get_int();
				s = size->get_int();
				o = old->get_int();
				if (!a) { message("HashtableEarlyIndirect override ignored: Address = 0?"); continue; }
				if (!s) { message("HashtableEarlyIndirect override ignored: NewSize = 0?"); continue; }
				if (!o) { message("HashtableEarlyIndirect override ignored: OldSize = 0?"); continue; }
#if defined NEW_VEGAS
				TextSection *version = ts->get_section("Version");
				const char *v = version ? version->get_c_string() : "";
				if (strcmp(v, Hook_target_s)) continue;
#endif
				Hashtable *ht = *(Hashtable **)a;
				if (!ht) {
					message("HashtableEarlyIndirect override ignored - pointer is nullptr (%x)", a);
					continue;
				}
				UInt32 actual_old = ht->m_numBuckets;
				if (o != actual_old) {
					message("HashtableEarlyIndirect override ignored - failed unsafe check (%d != %d)", o, actual_old);
					continue;
				}
				if (ht->m_numItems) {
					message("HashtableEarlyIndirect size override ignored - not empty (%X)", a);
					continue;
				}
				if (bEnableMessages) message("HashtableEarlyIndirect size override: %08X:%08X : %d -> %d", a, ht, o, s);
				ht->m_numBuckets = s; ht->m_buckets = (Hashtable::Entry**)malloc(4 * s); memset(ht->m_buckets, 0, 4 * s);
				num_hashtable_overrides++;
			}
		}
		if (num_hashtable_overrides) message("used %d hashtable size overrides", num_hashtable_overrides);
	}
}
