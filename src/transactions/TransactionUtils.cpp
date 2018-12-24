// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TransactionUtils.h"
#include "crypto/SecretKey.h"
#include "ledger/LedgerState.h"
#include "ledger/LedgerStateEntry.h"
#include "ledger/LedgerStateHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/OfferExchange.h"
#include "util/XDROperators.h"
#include "util/types.h"

namespace spn
{

LedgerStateEntry
loadAccount(AbstractLedgerState& ls, AccountID const& accountID)
{
    LedgerKey key(ACCOUNT);
    key.account().accountID = accountID;
    return ls.load(key);
}

ConstLedgerStateEntry
loadAccountWithoutRecord(AbstractLedgerState& ls, AccountID const& accountID)
{
    LedgerKey key(ACCOUNT);
    key.account().accountID = accountID;
    return ls.loadWithoutRecord(key);
}

LedgerStateEntry
loadData(AbstractLedgerState& ls, AccountID const& accountID,
         std::string const& dataName)
{
    LedgerKey key(DATA);
    key.data().accountID = accountID;
    key.data().dataName = dataName;
    return ls.load(key);
}

LedgerStateEntry
loadOffer(AbstractLedgerState& ls, AccountID const& sellerID, uint64_t offerID)
{
    LedgerKey key(OFFER);
    key.offer().sellerID = sellerID;
    key.offer().offerID = offerID;
    return ls.load(key);
}

TrustLineWrapper
loadTrustLine(AbstractLedgerState& ls, AccountID const& accountID,
              Asset const& asset)
{
    return TrustLineWrapper(ls, accountID, asset);
}

ConstTrustLineWrapper
loadTrustLineWithoutRecord(AbstractLedgerState& ls, AccountID const& accountID,
                           Asset const& asset)
{
    return ConstTrustLineWrapper(ls, accountID, asset);
}

TrustLineWrapper
loadTrustLineIfNotNative(AbstractLedgerState& ls, AccountID const& accountID,
                         Asset const& asset)
{
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        return {};
    }
    return TrustLineWrapper(ls, accountID, asset);
}

ConstTrustLineWrapper
loadTrustLineWithoutRecordIfNotNative(AbstractLedgerState& ls,
                                      AccountID const& accountID,
                                      Asset const& asset)
{
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        return {};
    }
    return ConstTrustLineWrapper(ls, accountID, asset);
}

static void
acquireOrReleaseLiabilities(AbstractLedgerState& ls,
                            LedgerStateHeader const& header,
                            LedgerStateEntry const& offerEntry, bool isAcquire)
{
    // This should never happen
    auto const& offer = offerEntry.current().data.offer();
    if (offer.buying == offer.selling)
    {
        throw std::runtime_error("buying and selling same asset");
    }
    auto const& sellerID = offer.sellerID;

    auto loadAccountAndValidate = [&ls, &sellerID]() {
        auto account = spn::loadAccount(ls, sellerID);
        if (!account)
        {
            throw std::runtime_error("account does not exist");
        }
        return account;
    };

    auto loadTrustAndValidate = [&ls, &sellerID](Asset const& asset) {
        auto trust = spn::loadTrustLine(ls, sellerID, asset);
        if (!trust)
        {
            throw std::runtime_error("trustline does not exist");
        }
        return trust;
    };

    int64_t buyingLiabilities =
        isAcquire ? getOfferBuyingLiabilities(header, offerEntry)
                  : -getOfferBuyingLiabilities(header, offerEntry);
    if (offer.buying.type() == ASSET_TYPE_NATIVE)
    {
        auto account = loadAccountAndValidate();
        if (!addBuyingLiabilities(header, account, buyingLiabilities))
        {
            throw std::runtime_error("could not add buying liabilities");
        }
    }
    else
    {
        auto buyingTrust = loadTrustAndValidate(offer.buying);
        if (!buyingTrust.addBuyingLiabilities(header, buyingLiabilities))
        {
            throw std::runtime_error("could not add buying liabilities");
        }
    }

    int64_t sellingLiabilities =
        isAcquire ? getOfferSellingLiabilities(header, offerEntry)
                  : -getOfferSellingLiabilities(header, offerEntry);
    if (offer.selling.type() == ASSET_TYPE_NATIVE)
    {
        auto account = loadAccountAndValidate();
        if (!addSellingLiabilities(header, account, sellingLiabilities))
        {
            throw std::runtime_error("could not add selling liabilities");
        }
    }
    else
    {
        auto sellingTrust = loadTrustAndValidate(offer.selling);
        if (!sellingTrust.addSellingLiabilities(header, sellingLiabilities))
        {
            throw std::runtime_error("could not add selling liabilities");
        }
    }
}

void
acquireLiabilities(AbstractLedgerState& ls, LedgerStateHeader const& header,
                   LedgerStateEntry const& offer)
{
    acquireOrReleaseLiabilities(ls, header, offer, true);
}

bool
addBalance(LedgerStateHeader const& header, LedgerStateEntry& entry,
           int64_t delta)
{
    if (entry.current().data.type() == ACCOUNT)
    {
        if (delta == 0)
        {
            return true;
        }

        auto& acc = entry.current().data.account();
        auto newBalance = acc.balance;
        if (!spn::addBalance(newBalance, delta))
        {
            return false;
        }
        if (header.current().ledgerVersion >= 10)
        {
            auto minBalance = getMinBalance(header, acc.numSubEntries);
            if (delta < 0 &&
                newBalance - minBalance < getSellingLiabilities(header, entry))
            {
                return false;
            }
            if (newBalance > INT64_MAX - getBuyingLiabilities(header, entry))
            {
                return false;
            }
        }

        acc.balance = newBalance;
        return true;
    }
    else if (entry.current().data.type() == TRUSTLINE)
    {
        if (delta == 0)
        {
            return true;
        }
        if (!isAuthorized(entry))
        {
            return false;
        }

        auto& tl = entry.current().data.trustLine();
        auto newBalance = tl.balance;
        if (!spn::addBalance(newBalance, delta, tl.limit))
        {
            return false;
        }
        if (header.current().ledgerVersion >= 10)
        {
            if (newBalance < getSellingLiabilities(header, entry))
            {
                return false;
            }
            if (newBalance > tl.limit - getBuyingLiabilities(header, entry))
            {
                return false;
            }
        }

        tl.balance = newBalance;
        return true;
    }
    else
    {
        throw std::runtime_error("Unknown LedgerEntry type");
    }
}

bool
addBuyingLiabilities(LedgerStateHeader const& header, LedgerStateEntry& entry,
                     int64_t delta)
{
    int64_t buyingLiab = getBuyingLiabilities(header, entry);

    // Fast-succeed when not actually adding any liabilities
    if (delta == 0)
    {
        return true;
    }

    if (entry.current().data.type() == ACCOUNT)
    {
        auto& acc = entry.current().data.account();

        int64_t maxLiabilities = INT64_MAX - acc.balance;
        bool res = spn::addBalance(buyingLiab, delta, maxLiabilities);
        if (res)
        {
            if (acc.ext.v() == 0)
            {
                acc.ext.v(1);
                acc.ext.v1().liabilities = Liabilities{0, 0};
            }
            acc.ext.v1().liabilities.buying = buyingLiab;
        }
        return res;
    }
    else if (entry.current().data.type() == TRUSTLINE)
    {
        auto& tl = entry.current().data.trustLine();
        if (!isAuthorized(entry))
        {
            return false;
        }

        int64_t maxLiabilities = tl.limit - tl.balance;
        bool res = spn::addBalance(buyingLiab, delta, maxLiabilities);
        if (res)
        {
            if (tl.ext.v() == 0)
            {
                tl.ext.v(1);
                tl.ext.v1().liabilities = Liabilities{0, 0};
            }
            tl.ext.v1().liabilities.buying = buyingLiab;
        }
        return res;
    }
    else
    {
        throw std::runtime_error("Unknown LedgerEntry type");
    }
}

bool
addNumEntries(LedgerStateHeader const& header, LedgerStateEntry& entry,
              int count)
{
    auto& acc = entry.current().data.account();
    int newEntriesCount = acc.numSubEntries + count;
    if (newEntriesCount < 0)
    {
        throw std::runtime_error("invalid account state");
    }

    int64_t effMinBalance = getMinBalance(header, newEntriesCount);
    if (header.current().ledgerVersion >= 10)
    {
        effMinBalance += getSellingLiabilities(header, entry);
    }

    // only check minBalance when attempting to add subEntries
    if (count > 0 && acc.balance < effMinBalance)
    {
        // balance too low
        return false;
    }
    acc.numSubEntries = newEntriesCount;
    return true;
}

bool
addSellingLiabilities(LedgerStateHeader const& header, LedgerStateEntry& entry,
                      int64_t delta)
{
    int64_t sellingLiab = getSellingLiabilities(header, entry);

    // Fast-succeed when not actually adding any liabilities
    if (delta == 0)
    {
        return true;
    }

    if (entry.current().data.type() == ACCOUNT)
    {
        auto& acc = entry.current().data.account();
        int64_t maxLiabilities =
            acc.balance - getMinBalance(header, acc.numSubEntries);
        if (maxLiabilities < 0)
        {
            return false;
        }

        bool res = spn::addBalance(sellingLiab, delta, maxLiabilities);
        if (res)
        {
            if (acc.ext.v() == 0)
            {
                acc.ext.v(1);
                acc.ext.v1().liabilities = Liabilities{0, 0};
            }
            acc.ext.v1().liabilities.selling = sellingLiab;
        }
        return res;
    }
    else if (entry.current().data.type() == TRUSTLINE)
    {
        auto& tl = entry.current().data.trustLine();
        if (!isAuthorized(entry))
        {
            return false;
        }

        int64_t maxLiabilities = tl.balance;
        bool res = spn::addBalance(sellingLiab, delta, maxLiabilities);
        if (res)
        {
            if (tl.ext.v() == 0)
            {
                tl.ext.v(1);
                tl.ext.v1().liabilities = Liabilities{0, 0};
            }
            tl.ext.v1().liabilities.selling = sellingLiab;
        }
        return res;
    }
    else
    {
        throw std::runtime_error("Unknown LedgerEntry type");
    }
}

uint64_t
generateID(LedgerStateHeader& header)
{
    return ++header.current().idPool;
}

int64_t
getAvailableBalance(LedgerStateHeader const& header, LedgerEntry const& le)
{
    int64_t avail = 0;
    if (le.data.type() == ACCOUNT)
    {
        auto const& acc = le.data.account();
        avail = acc.balance - getMinBalance(header, acc.numSubEntries);
    }
    else if (le.data.type() == TRUSTLINE)
    {
        avail = le.data.trustLine().balance;
    }
    else
    {
        throw std::runtime_error("Unknown LedgerEntry type");
    }

    if (header.current().ledgerVersion >= 10)
    {
        avail -= getSellingLiabilities(header, le);
    }
    return avail;
}

int64_t
getAvailableBalance(LedgerStateHeader const& header,
                    LedgerStateEntry const& entry)
{
    return getAvailableBalance(header, entry.current());
}

int64_t
getAvailableBalance(LedgerStateHeader const& header,
                    ConstLedgerStateEntry const& entry)
{
    return getAvailableBalance(header, entry.current());
}

int64_t
getBuyingLiabilities(LedgerStateHeader const& header, LedgerEntry const& le)
{
    if (header.current().ledgerVersion < 10)
    {
        throw std::runtime_error("Liabilities accessed before version 10");
    }

    if (le.data.type() == ACCOUNT)
    {
        auto const& acc = le.data.account();
        return (acc.ext.v() == 0) ? 0 : acc.ext.v1().liabilities.buying;
    }
    else if (le.data.type() == TRUSTLINE)
    {
        auto const& tl = le.data.trustLine();
        return (tl.ext.v() == 0) ? 0 : tl.ext.v1().liabilities.buying;
    }
    throw std::runtime_error("Unknown LedgerEntry type");
}

int64_t
getBuyingLiabilities(LedgerStateHeader const& header,
                     LedgerStateEntry const& entry)
{
    return getBuyingLiabilities(header, entry.current());
}

int64_t
getMaxAmountReceive(LedgerStateHeader const& header, LedgerEntry const& le)
{
    if (le.data.type() == ACCOUNT)
    {
        int64_t maxReceive = INT64_MAX;
        if (header.current().ledgerVersion >= 10)
        {
            auto const& acc = le.data.account();
            maxReceive -= acc.balance + getBuyingLiabilities(header, le);
        }
        return maxReceive;
    }
    if (le.data.type() == TRUSTLINE)
    {
        int64_t amount = 0;
        if (isAuthorized(le))
        {
            auto const& tl = le.data.trustLine();
            amount = tl.limit - tl.balance;
            if (header.current().ledgerVersion >= 10)
            {
                amount -= getBuyingLiabilities(header, le);
            }
        }
        return amount;
    }
    else
    {
        throw std::runtime_error("Unknown LedgerEntry type");
    }
}

int64_t
getMaxAmountReceive(LedgerStateHeader const& header,
                    LedgerStateEntry const& entry)
{
    return getMaxAmountReceive(header, entry.current());
}

int64_t
getMaxAmountReceive(LedgerStateHeader const& header,
                    ConstLedgerStateEntry const& entry)
{
    return getMaxAmountReceive(header, entry.current());
}

int64_t
getMinBalance(LedgerStateHeader const& header, uint32_t ownerCount)
{
    auto const& lh = header.current();
    if (lh.ledgerVersion <= 8)
        return (2 + ownerCount) * lh.baseReserve;
    else
        return (2 + ownerCount) * int64_t(lh.baseReserve);
}

int64_t
getMinimumLimit(LedgerStateHeader const& header, LedgerEntry const& le)
{
    auto const& tl = le.data.trustLine();
    int64_t minLimit = tl.balance;
    if (header.current().ledgerVersion >= 10)
    {
        minLimit += getBuyingLiabilities(header, le);
    }
    return minLimit;
}

int64_t
getMinimumLimit(LedgerStateHeader const& header, LedgerStateEntry const& entry)
{
    return getMinimumLimit(header, entry.current());
}

int64_t
getMinimumLimit(LedgerStateHeader const& header,
                ConstLedgerStateEntry const& entry)
{
    return getMinimumLimit(header, entry.current());
}

int64_t
getOfferBuyingLiabilities(LedgerStateHeader const& header,
                          LedgerEntry const& entry)
{
    if (header.current().ledgerVersion < 10)
    {
        throw std::runtime_error(
            "Offer liabilities calculated before version 10");
    }
    auto const& oe = entry.data.offer();
    auto res = exchangeV10WithoutPriceErrorThresholds(
        oe.price, oe.amount, INT64_MAX, INT64_MAX, INT64_MAX, false);
    return res.numSheepSend;
}

int64_t
getOfferBuyingLiabilities(LedgerStateHeader const& header,
                          LedgerStateEntry const& entry)
{
    return getOfferBuyingLiabilities(header, entry.current());
}

int64_t
getOfferSellingLiabilities(LedgerStateHeader const& header,
                           LedgerEntry const& entry)
{
    if (header.current().ledgerVersion < 10)
    {
        throw std::runtime_error(
            "Offer liabilities calculated before version 10");
    }
    auto const& oe = entry.data.offer();
    auto res = exchangeV10WithoutPriceErrorThresholds(
        oe.price, oe.amount, INT64_MAX, INT64_MAX, INT64_MAX, false);
    return res.numWheatReceived;
}

int64_t
getOfferSellingLiabilities(LedgerStateHeader const& header,
                           LedgerStateEntry const& entry)
{
    return getOfferSellingLiabilities(header, entry.current());
}

int64_t
getSellingLiabilities(LedgerStateHeader const& header, LedgerEntry const& le)
{
    if (header.current().ledgerVersion < 10)
    {
        throw std::runtime_error("Liabilities accessed before version 10");
    }

    if (le.data.type() == ACCOUNT)
    {
        auto const& acc = le.data.account();
        return (acc.ext.v() == 0) ? 0 : acc.ext.v1().liabilities.selling;
    }
    else if (le.data.type() == TRUSTLINE)
    {
        auto const& tl = le.data.trustLine();
        return (tl.ext.v() == 0) ? 0 : tl.ext.v1().liabilities.selling;
    }
    throw std::runtime_error("Unknown LedgerEntry type");
}

int64_t
getSellingLiabilities(LedgerStateHeader const& header,
                      LedgerStateEntry const& entry)
{
    return getSellingLiabilities(header, entry.current());
}

uint64_t
getStartingSequenceNumber(LedgerStateHeader const& header)
{
    return static_cast<uint64_t>(header.current().ledgerSeq) << 32;
}

bool
isAuthorized(LedgerEntry const& le)
{
    return (le.data.trustLine().flags & AUTHORIZED_FLAG) != 0;
}

bool
isAuthorized(LedgerStateEntry const& entry)
{
    return isAuthorized(entry.current());
}

bool
isAuthorized(ConstLedgerStateEntry const& entry)
{
    return isAuthorized(entry.current());
}

bool
isAuthRequired(ConstLedgerStateEntry const& entry)
{
    return (entry.current().data.account().flags & AUTH_REQUIRED_FLAG) != 0;
}

bool
isImmutableAuth(LedgerStateEntry const& entry)
{
    return (entry.current().data.account().flags & AUTH_IMMUTABLE_FLAG) != 0;
}

void
normalizeSigners(LedgerStateEntry& entry)
{
    auto& acc = entry.current().data.account();
    std::sort(
        acc.signers.begin(), acc.signers.end(),
        [](Signer const& s1, Signer const& s2) { return s1.key < s2.key; });
}

void
releaseLiabilities(AbstractLedgerState& ls, LedgerStateHeader const& header,
                   LedgerStateEntry const& offer)
{
    acquireOrReleaseLiabilities(ls, header, offer, false);
}

void
setAuthorized(LedgerStateEntry& entry, bool authorized)
{
    auto& tl = entry.current().data.trustLine();
    if (authorized)
    {
        tl.flags |= AUTHORIZED_FLAG;
    }
    else
    {
        tl.flags &= ~AUTHORIZED_FLAG;
    }
}
}
