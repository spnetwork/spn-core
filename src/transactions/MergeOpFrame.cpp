// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/MergeOpFrame.h"
#include "database/Database.h"
#include "ledger/LedgerState.h"
#include "ledger/LedgerStateEntry.h"
#include "ledger/LedgerStateHeader.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/XDROperators.h"

using namespace soci;

namespace spn
{

MergeOpFrame::MergeOpFrame(Operation const& op, OperationResult& res,
                           TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
{
}

ThresholdLevel
MergeOpFrame::getThresholdLevel() const
{
    return ThresholdLevel::HIGH;
}

// make sure the deleted Account hasn't issued credit
// make sure we aren't holding any credit
// make sure the we delete all the offers
// make sure the we delete all the trustlines
// move the XLM to the new account
bool
MergeOpFrame::doApply(Application& app, AbstractLedgerState& ls)
{
    auto header = ls.loadHeader();

    auto otherAccount = spn::loadAccount(ls, mOperation.body.destination());
    if (!otherAccount)
    {
        app.getMetrics()
            .NewMeter({"op-merge", "failure", "no-account"}, "operation")
            .Mark();
        innerResult().code(ACCOUNT_MERGE_NO_ACCOUNT);
        return false;
    }

    int64_t sourceBalance = 0;
    if (header.current().ledgerVersion > 4 &&
        header.current().ledgerVersion < 8)
    {
        // in versions < 8, merge account could be called with a stale account
        LedgerKey key(ACCOUNT);
        key.account().accountID = getSourceID();
        auto thisAccount = ls.loadWithoutRecord(key);
        if (!thisAccount)
        {
            app.getMetrics()
                .NewMeter({"op-merge", "failure", "no-account"}, "operation")
                .Mark();
            innerResult().code(ACCOUNT_MERGE_NO_ACCOUNT);
            return false;
        }

        if (header.current().ledgerVersion > 5)
        {
            sourceBalance = thisAccount.current().data.account().balance;
        }
    }

    auto sourceAccountEntry = loadSourceAccount(ls, header);
    auto const& sourceAccount = sourceAccountEntry.current().data.account();
    // Only set sourceBalance here if it wasn't set in the previous block
    if (header.current().ledgerVersion <= 5 ||
        header.current().ledgerVersion >= 8)
    {
        sourceBalance = sourceAccount.balance;
    }

    if (isImmutableAuth(sourceAccountEntry))
    {
        app.getMetrics()
            .NewMeter({"op-merge", "failure", "static-auth"}, "operation")
            .Mark();
        innerResult().code(ACCOUNT_MERGE_IMMUTABLE_SET);
        return false;
    }

    if (sourceAccount.numSubEntries != sourceAccount.signers.size())
    {
        app.getMetrics()
            .NewMeter({"op-merge", "failure", "has-sub-entries"}, "operation")
            .Mark();
        innerResult().code(ACCOUNT_MERGE_HAS_SUB_ENTRIES);
        return false;
    }

    if (header.current().ledgerVersion >= 10)
    {
        SequenceNumber maxSeq = getStartingSequenceNumber(header);

        // don't allow the account to be merged if recreating it would cause it
        // to jump backwards
        if (sourceAccount.seqNum >= maxSeq)
        {
            app.getMetrics()
                .NewMeter({"op-merge", "failure", "too-far"}, "operation")
                .Mark();
            innerResult().code(ACCOUNT_MERGE_SEQNUM_TOO_FAR);
            return false;
        }
    }

    // "success" path starts
    if (!addBalance(header, otherAccount, sourceBalance))
    {
        app.getMetrics()
            .NewMeter({"op-merge", "failure", "dest-full"}, "operation")
            .Mark();
        innerResult().code(ACCOUNT_MERGE_DEST_FULL);
        return false;
    }

    sourceAccountEntry.erase();

    app.getMetrics()
        .NewMeter({"op-merge", "success", "apply"}, "operation")
        .Mark();
    innerResult().code(ACCOUNT_MERGE_SUCCESS);
    innerResult().sourceAccountBalance() = sourceBalance;
    return true;
}

bool
MergeOpFrame::doCheckValid(Application& app, uint32_t ledgerVersion)
{
    // makes sure not merging into self
    if (getSourceID() == mOperation.body.destination())
    {
        app.getMetrics()
            .NewMeter({"op-merge", "invalid", "malformed-self-merge"},
                      "operation")
            .Mark();
        innerResult().code(ACCOUNT_MERGE_MALFORMED);
        return false;
    }
    return true;
}
}
