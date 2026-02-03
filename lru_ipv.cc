#include "mem/cache/replacement_policies/lru_ipv.hh"

#include <limits>

#include "base/logging.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"

// ---------------- Small utilities ----------------

void
LRUIPVRP::ensureSet(uint32_t set) const
{
    auto &v = setAges[set];
    if ((int)v.size() < numWays) {
        v.resize(numWays, 0);
        // Nice ascending initial state for first printouts
        for (int i = 0; i < numWays; ++i) v[i] = i;
    }
}

void
LRUIPVRP::printAges(const std::vector<uint64_t>& v)
{
    for (size_t i = 0; i < v.size(); ++i) {
        std::printf("%llu", static_cast<unsigned long long>(v[i]));
        if (i + 1 < v.size()) std::printf(" ");
    }
}

void
LRUIPVRP::normalize(std::vector<uint64_t>& v)
{
    // Stable sort indices by age, then relabel to 0..N-1
    std::vector<int> idx(v.size());
    for (size_t i = 0; i < v.size(); ++i) idx[i] = static_cast<int>(i);
    std::stable_sort(idx.begin(), idx.end(),
                     [&](int a, int b){ return v[a] < v[b]; });
    uint64_t a = 0;
    for (int i : idx) v[i] = a++;
}

uint64_t
LRUIPVRP::currentMRU(const std::vector<uint64_t>& v)
{
    uint64_t m = 0;
    for (auto x : v) m = std::max(m, x);
    return m;
}

uint64_t
LRUIPVRP::promoteToMRU(std::vector<uint64_t>& v, int way)
{
    const uint64_t old = v[way];
    const uint64_t mru = currentMRU(v);
    // Decrement entries that were newer than 'old' to keep order compact.
    for (size_t i = 0; i < v.size(); ++i) {
        if ((int)i == way) continue;
        if (v[i] > old) v[i] -= 1;
    }
    v[way] = mru;
    // normalize(v);
    return v[way];
}

uint64_t
LRUIPVRP::insertNearLRU(std::vector<uint64_t>& v, int way)
{
    // Put target at LRU (0) and bump others, then compact
    for (size_t i = 0; i < v.size(); ++i) {
        if ((int)i == way) continue;
        v[i] += 1;
    }
    v[way] = 0;
    // normalize(v);
    return v[way];
}

// --------------- Policy implementation ----------------

LRUIPVRP::LRUIPVRP(const LRUIPVRPParams &p)
    : ReplacementPolicy::Base(p),
      numWays(p.numWays),
      mruPct(p.mru_pct),
      quantum(std::max(1, p.quantum)),
      pv(quantum, 0),
      insPos(0)
{
    fatal_if(numWays <= 0, "LRUIPVRP: numWays must be > 0");
    // IPV schedule: first (quantum*mruPct/100) are MRU inserts
    const int mru_count = std::max(0, std::min(quantum, (quantum * mruPct) / 100));
    for (int i = 0; i < mru_count; ++i) pv[i] = 1;
}

std::shared_ptr<ReplacementPolicy::ReplacementData>
LRUIPVRP::instantiateEntry()
{
    return std::make_shared<IPVReplData>();
}

void
LRUIPVRP::invalidate(const std::shared_ptr<ReplacementPolicy::ReplacementData>& rdata) const
{
    auto d = std::static_pointer_cast<IPVReplData>(rdata);
    d->valid = false;
    d->age = 0;
    // set/way left as-is (harmless)
}

void
LRUIPVRP::touch(const std::shared_ptr<ReplacementPolicy::ReplacementData>& rdata) const
{
    // Hit: promote to MRU and print transition
    auto d = std::static_pointer_cast<IPVReplData>(rdata);
    const uint32_t set = d->set;
    const int      way = static_cast<int>(d->way);

    ensureSet(set);
    auto &v = setAges[set];

    std::printf("\nIn touch.\n");
    std::printf("\tSetID: %u\tindex: %d\n", set, way);

    std::printf("\told sharedState: ");
    printAges(v);
    std::printf("  New sharedState is: ");

    promoteToMRU(v, way);
    printAges(v);
    std::printf(" \n");

    d->age = v[way];
    d->valid = true;
}

void
LRUIPVRP::reset(const std::shared_ptr<ReplacementPolicy::ReplacementData>& rdata) const
{
    // Insertion after miss: use IPV schedule (MRU vs near-LRU) and print
    // NOTE: getVictim() already populated rdata->set/way correctly.
    auto d = std::static_pointer_cast<IPVReplData>(rdata);
    const uint32_t set = d->set;
    const int      way = static_cast<int>(d->way);

    ensureSet(set);
    auto &v = setAges[set];

    std::printf("\nIn reset.\n");
    std::printf("\tSetID: %u\tindex: %d\n", set, way);

    std::printf("\told sharedState: ");
    printAges(v);
    std::printf("  New sharedState is: ");

    const bool insertMRU = (pv[insPos] == 1);
    insPos = (insPos + 1) % quantum;

    const uint64_t new_age = insertMRU ? promoteToMRU(v, way)
                                       : insertNearLRU(v, way);

    printAges(v);
    std::printf(" \n");

    d->age = new_age;
    d->valid = true;
}

ReplaceableEntry*
LRUIPVRP::getVictim(const ReplacementCandidates& candidates) const
{
    panic_if(candidates.empty(), "No candidates to select a victim from!");

    // Candidates are all from the same set
    auto *any_entry = candidates[0];
    const uint32_t set = any_entry->getSet();

    // IMPORTANT: populate (set,way) into each candidate's rdata so that
    // subsequent reset()/touch() have correct IDs without pointer tricks.
    for (auto *e : candidates) {
        auto d = std::static_pointer_cast<IPVReplData>(e->replacementData);
        d->set = e->getSet();
        d->way = e->getWay();
        d->valid = true;
    }

    ensureSet(set);
    auto &v = setAges[set];

    // Warm-start sync: align our age vector with candidates' stored ages
    for (auto *e : candidates) {
        const int w = static_cast<int>(e->getWay());
        if (w >= 0 && w < numWays) {
            auto d = std::static_pointer_cast<IPVReplData>(e->replacementData);
            if (d) v[w] = d->age;
        }
    }
    normalize(v);

    // Choose LRU (minimal age)
    ReplaceableEntry* victim = candidates[0];
    uint64_t min_age = std::numeric_limits<uint64_t>::max();
    for (auto *e : candidates) {
        auto d = std::static_pointer_cast<IPVReplData>(e->replacementData);
        if (d && d->age <= min_age) {
            min_age = d->age;
            victim = e;
        }
    }

    // Required prints
    std::printf("In getVictim. SetID: %u\n", set);
    std::printf("In getVictim. sharedState is: ");
    printAges(v);
    std::printf("\t Victim: %u\n", victim->getWay());

    return victim;
}

