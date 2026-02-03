#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_LRU_IPV_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_LRU_IPV_HH__

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <vector>

#include "mem/cache/replacement_policies/base.hh"
#include "params/LRUIPVRP.hh"

/**
 * LRUIPVRP — LRU with IPV-style insertion and verbose prints.
 *
 * Design:
 * - Each set has a compact "age" vector (size = numWays). After normalize(),
 *   ages are 0..N-1 with 0 = LRU and N-1 = MRU.
 * - touch(): promote to MRU.
 * - reset(): insert at MRU or near-LRU depending on an IPV schedule.
 * - getVictim(): choose min age (LRU).
 *
 * Critical note (fixes constant SetID):
 * - We do NOT try to reconstruct ReplaceableEntry* from ReplacementData*.
 * - In getVictim() (where gem5 passes real entries), we record (set,way)
 *   into each candidate's metadata. Later, reset()/touch() read those fields.
 */
class LRUIPVRP : public ReplacementPolicy::Base
{
  public:
    struct IPVReplData : public ReplacementPolicy::ReplacementData
    {
        uint64_t age = 0;     ///< Larger value == more recent (MRU)
        bool     valid = false;
        uint32_t set = 0;     ///< Cache set id (written in getVictim())
        uint32_t way = 0;     ///< Way index within the set (written in getVictim())
    };

    explicit LRUIPVRP(const LRUIPVRPParams &p);

    std::shared_ptr<ReplacementPolicy::ReplacementData>
    instantiateEntry() override;

    void invalidate(const std::shared_ptr<ReplacementPolicy::ReplacementData>&) const override;
    void touch(const std::shared_ptr<ReplacementPolicy::ReplacementData>&) const override;
    void reset(const std::shared_ptr<ReplacementPolicy::ReplacementData>&) const override;
    ReplaceableEntry* getVictim(const ReplacementCandidates& candidates) const override;

  private:
    // ---- Config ----
    const int numWays;   ///< Set associativity
    const int mruPct;    ///< % (0..100) of MRU insertions within a quantum
    const int quantum;   ///< Schedule period length

    // IPV schedule: pv[i]==1 → insert MRU, 0 → insert near LRU
    mutable std::vector<int> pv;
    mutable int insPos = 0;

    // Per-set age vectors (dense order 0..numWays-1)
    mutable std::unordered_map<uint32_t, std::vector<uint64_t>> setAges;

    // ---- Helpers ----
    void        ensureSet(uint32_t set) const;
    static void printAges(const std::vector<uint64_t>& v);
    static void normalize(std::vector<uint64_t>& v);
    static uint64_t currentMRU(const std::vector<uint64_t>& v);
    static uint64_t promoteToMRU(std::vector<uint64_t>& v, int way);
    static uint64_t insertNearLRU(std::vector<uint64_t>& v, int way);
};

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_LRU_IPV_HH__

