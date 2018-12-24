// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/InvariantManagerImpl.h"
#include "bucket/Bucket.h"
#include "bucket/BucketList.h"
#include "crypto/Hex.h"
#include "invariant/Invariant.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManagerImpl.h"
#include "ledger/LedgerState.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "util/Logging.h"
#include "xdrpp/printer.h"

#include "medida/counter.h"
#include "medida/metrics_registry.h"

#include <memory>
#include <numeric>
#include <regex>

namespace spn
{

std::unique_ptr<InvariantManager>
InvariantManager::create(Application& app)
{
    return std::make_unique<InvariantManagerImpl>(app.getMetrics());
}

InvariantManagerImpl::InvariantManagerImpl(medida::MetricsRegistry& registry)
    : mMetricsRegistry(registry)
{
}

Json::Value
InvariantManagerImpl::getJsonInfo()
{
    Json::Value failures;
    for (auto const& invariant : mInvariants)
    {
        auto& counter = mMetricsRegistry.NewCounter(
            {"invariant", "does-not-hold", "count", invariant.first});
        if (counter.count() > 0)
        {
            auto const& info = mFailureInformation.at(invariant.first);

            auto& fail = failures[invariant.first];
            fail["count"] = (Json::Int64)counter.count();
            fail["last_failed_on_ledger"] = info.lastFailedOnLedger;
            fail["last_failed_with_message"] = info.lastFailedWithMessage;
        }
    }
    return failures;
}

std::vector<std::string>
InvariantManagerImpl::getEnabledInvariants() const
{
    std::vector<std::string> res;
    for (auto const& p : mEnabled)
    {
        res.emplace_back(p->getName());
    }
    return res;
}

void
InvariantManagerImpl::checkOnBucketApply(std::shared_ptr<Bucket const> bucket,
                                         uint32_t ledger, uint32_t level,
                                         bool isCurr)
{
    uint32_t oldestLedger = isCurr
                                ? BucketList::oldestLedgerInCurr(ledger, level)
                                : BucketList::oldestLedgerInSnap(ledger, level);
    uint32_t newestLedger = oldestLedger - 1 +
                            (isCurr ? BucketList::sizeOfCurr(ledger, level)
                                    : BucketList::sizeOfSnap(ledger, level));
    for (auto invariant : mEnabled)
    {
        auto result =
            invariant->checkOnBucketApply(bucket, oldestLedger, newestLedger);
        if (result.empty())
        {
            continue;
        }

        auto message = fmt::format(
            R"(invariant "{}" does not hold on bucket {}[{}] = {}: {})",
            invariant->getName(), isCurr ? "Curr" : "Snap", level,
            binToHex(bucket->getHash()), result);
        onInvariantFailure(invariant, message, ledger);
    }
}

void
InvariantManagerImpl::checkOnOperationApply(Operation const& operation,
                                            OperationResult const& opres,
                                            LedgerStateDelta const& lsDelta)
{
    if (lsDelta.header.current.ledgerVersion < 8)
    {
        return;
    }

    for (auto invariant : mEnabled)
    {
        auto result =
            invariant->checkOnOperationApply(operation, opres, lsDelta);
        if (result.empty())
        {
            continue;
        }

        auto message = fmt::format(
            R"(Invariant "{}" does not hold on operation: {}{}{})",
            invariant->getName(), result, "\n", xdr::xdr_to_string(operation));
        onInvariantFailure(invariant, message,
                           lsDelta.header.current.ledgerSeq);
    }
}

void
InvariantManagerImpl::registerInvariant(std::shared_ptr<Invariant> invariant)
{
    auto name = invariant->getName();
    auto iter = mInvariants.find(name);
    if (iter == mInvariants.end())
    {
        mInvariants[name] = invariant;
        mMetricsRegistry.NewCounter(
            {"invariant", "does-not-hold", "count", invariant->getName()});
    }
    else
    {
        throw std::runtime_error{"Invariant " + invariant->getName() +
                                 " already registered"};
    }
}

void
InvariantManagerImpl::enableInvariant(std::string const& invPattern)
{
    if (invPattern.empty())
    {
        throw std::invalid_argument("Invariant pattern must be non empty");
    }

    std::regex r;
    try
    {
        r = std::regex(invPattern, std::regex::ECMAScript | std::regex::icase);
    }
    catch (std::regex_error& e)
    {
        throw std::invalid_argument(fmt::format(
            "Invalid invariant pattern '{}': {}", invPattern, e.what()));
    }

    bool enabledSome = false;
    for (auto const& inv : mInvariants)
    {
        auto const& name = inv.first;
        if (std::regex_match(name, r, std::regex_constants::match_not_null))
        {
            auto iter = std::find(mEnabled.begin(), mEnabled.end(), inv.second);
            if (iter == mEnabled.end())
            {
                enabledSome = true;
                mEnabled.push_back(inv.second);
                CLOG(INFO, "Invariant") << "Enabled invariant '" << name << "'";
            }
            else
            {
                throw std::runtime_error{"Invariant " + name +
                                         " already enabled"};
            }
        }
    }
    if (!enabledSome)
    {
        std::string message = fmt::format(
            "Invariant pattern '{}' did not match any invariants.", invPattern);
        if (mInvariants.size() > 0)
        {
            using value_type = decltype(mInvariants)::value_type;
            std::string registered = std::accumulate(
                std::next(mInvariants.cbegin()), mInvariants.cend(),
                mInvariants.cbegin()->first,
                [](std::string const& lhs, value_type const& rhs) {
                    return lhs + ", " + rhs.first;
                });
            message += " Registered invariants are: " + registered;
        }
        else
        {
            message += " There are no registered invariants";
        }
        throw std::runtime_error{message};
    }
}

void
InvariantManagerImpl::onInvariantFailure(std::shared_ptr<Invariant> invariant,
                                         std::string const& message,
                                         uint32_t ledger)
{
    mMetricsRegistry
        .NewCounter(
            {"invariant", "does-not-hold", "count", invariant->getName()})
        .inc();
    mFailureInformation[invariant->getName()].lastFailedOnLedger = ledger;
    mFailureInformation[invariant->getName()].lastFailedWithMessage = message;
    handleInvariantFailure(invariant, message);
}

void
InvariantManagerImpl::handleInvariantFailure(
    std::shared_ptr<Invariant> invariant, std::string const& message) const
{
    if (invariant->isStrict())
    {
        CLOG(FATAL, "Invariant") << message;
        throw InvariantDoesNotHold{message};
    }
    else
    {
        CLOG(ERROR, "Invariant") << message;
    }
}
}
