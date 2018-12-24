// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/HistoryManager.h"
#include "ledger/LedgerRange.h"
#include "work/Work.h"

namespace medida
{
class Meter;
}

namespace spn
{

class TmpDir;
struct LedgerHeaderHistoryEntry;

class VerifyLedgerChainWork : public Work
{
    TmpDir const& mDownloadDir;
    LedgerRange mRange;
    uint32_t mCurrCheckpoint;
    bool mManualCatchup;
    LedgerHeaderHistoryEntry& mFirstVerified;
    LedgerHeaderHistoryEntry& mLastVerified;

    medida::Meter& mVerifyLedgerSuccess;
    medida::Meter& mVerifyLedgerChainSuccess;
    medida::Meter& mVerifyLedgerChainFailure;

    HistoryManager::LedgerVerificationStatus verifyHistoryOfSingleCheckpoint();

  public:
    VerifyLedgerChainWork(Application& app, WorkParent& parent,
                          TmpDir const& downloadDir, LedgerRange range,
                          bool manualCatchup,
                          LedgerHeaderHistoryEntry& firstVerified,
                          LedgerHeaderHistoryEntry& lastVerified);
    ~VerifyLedgerChainWork();
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;
};
}
