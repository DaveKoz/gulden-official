// Copyright (c) 2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// File contains modifications by: The Gulden developers
// All modifications:
// Copyright (c) 2016 The Gulden developers
// Authored by: Malcolm MacLeod (mmacleod@webmail.co.za)
// Distributed under the GULDEN software license, see the accompanying
// file COPYING

#include "versionbits.h"

#include "consensus/params.h"

const struct BIP9DeploymentInfo VersionBitsDeploymentInfo[Consensus::MAX_VERSION_BITS_DEPLOYMENTS] = {
    { /*.name =*/"testdummy",
      /*.gbt_force =*/true,
    },
    { /*.name =*/"csv",
      /*.gbt_force =*/true,
    },
    { /*.name =*/"segwit",
      /*.gbt_force =*/false,
    }
};

ThresholdState AbstractThresholdConditionChecker::GetStateFor(const CBlockIndex* pindexPrev, const Consensus::Params& params, ThresholdConditionCache& cache) const
{
    int nPeriod = Period(params);
    int nThreshold = Threshold(params);
    int64_t nTimeStart = BeginTime(params);
    int64_t nTimeTimeout = EndTime(params);

    if (pindexPrev != NULL) {
        pindexPrev = pindexPrev->GetAncestor(pindexPrev->nHeight - ((pindexPrev->nHeight + 1) % nPeriod));
    }

    std::vector<const CBlockIndex*> vToCompute;
    while (cache.count(pindexPrev) == 0) {
        if (pindexPrev == NULL) {

            cache[pindexPrev] = THRESHOLD_DEFINED;
            break;
        }
        if (pindexPrev->GetMedianTimePast(pindexPrev->nHeight) < nTimeStart) {

            cache[pindexPrev] = THRESHOLD_DEFINED;
            break;
        }
        vToCompute.push_back(pindexPrev);
        pindexPrev = pindexPrev->GetAncestor(pindexPrev->nHeight - nPeriod);
    }

    assert(cache.count(pindexPrev));
    ThresholdState state = cache[pindexPrev];

    while (!vToCompute.empty()) {
        ThresholdState stateNext = state;
        pindexPrev = vToCompute.back();
        vToCompute.pop_back();

        switch (state) {
        case THRESHOLD_DEFINED: {
            if (pindexPrev->GetMedianTimePast(pindexPrev->nHeight) >= nTimeTimeout) {
                stateNext = THRESHOLD_FAILED;
            } else if (pindexPrev->GetMedianTimePast(pindexPrev->nHeight) >= nTimeStart) {
                stateNext = THRESHOLD_STARTED;
            }
            break;
        }
        case THRESHOLD_STARTED: {
            if (pindexPrev->GetMedianTimePast(pindexPrev->nHeight) >= nTimeTimeout) {
                stateNext = THRESHOLD_FAILED;
                break;
            }

            const CBlockIndex* pindexCount = pindexPrev;
            int count = 0;
            for (int i = 0; i < nPeriod; i++) {
                if (Condition(pindexCount, params)) {
                    count++;
                }
                pindexCount = pindexCount->pprev;
            }
            if (count >= nThreshold) {
                stateNext = THRESHOLD_LOCKED_IN;
            }
            break;
        }
        case THRESHOLD_LOCKED_IN: {

            stateNext = THRESHOLD_ACTIVE;
            break;
        }
        case THRESHOLD_FAILED:
        case THRESHOLD_ACTIVE: {

            break;
        }
        }
        cache[pindexPrev] = state = stateNext;
    }

    return state;
}

namespace {
/**
 * Class to implement versionbits logic.
 */
class VersionBitsConditionChecker : public AbstractThresholdConditionChecker {
private:
    const Consensus::DeploymentPos id;

protected:
    int64_t BeginTime(const Consensus::Params& params) const { return params.vDeployments[id].nStartTime; }
    int64_t EndTime(const Consensus::Params& params) const { return params.vDeployments[id].nTimeout; }
    int Period(const Consensus::Params& params) const { return params.nMinerConfirmationWindow; }
    int Threshold(const Consensus::Params& params) const { return params.nRuleChangeActivationThreshold; }

    bool Condition(const CBlockIndex* pindex, const Consensus::Params& params) const
    {
        return (((pindex->nVersion & VERSIONBITS_TOP_MASK) == VERSIONBITS_TOP_BITS) && (pindex->nVersion & Mask(params)) != 0);
    }

public:
    VersionBitsConditionChecker(Consensus::DeploymentPos id_)
        : id(id_)
    {
    }
    uint32_t Mask(const Consensus::Params& params) const { return ((uint32_t)1) << params.vDeployments[id].bit; }
};
}

ThresholdState VersionBitsState(const CBlockIndex* pindexPrev, const Consensus::Params& params, Consensus::DeploymentPos pos, VersionBitsCache& cache)
{
    return VersionBitsConditionChecker(pos).GetStateFor(pindexPrev, params, cache.caches[pos]);
}

uint32_t VersionBitsMask(const Consensus::Params& params, Consensus::DeploymentPos pos)
{
    return VersionBitsConditionChecker(pos).Mask(params);
}

void VersionBitsCache::Clear()
{
    for (unsigned int d = 0; d < Consensus::MAX_VERSION_BITS_DEPLOYMENTS; d++) {
        caches[d].clear();
    }
}
