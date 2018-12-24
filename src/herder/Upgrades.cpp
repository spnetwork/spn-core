// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Upgrades.h"
#include "database/Database.h"
#include "database/DatabaseUtils.h"
#include "ledger/LedgerState.h"
#include "ledger/LedgerStateEntry.h"
#include "ledger/LedgerStateHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "main/Config.h"
#include "transactions/OfferExchange.h"
#include "transactions/TransactionUtils.h"
#include "util/Decoder.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/types.h"
#include <cereal/archives/json.hpp>
#include <cereal/cereal.hpp>
#include <lib/util/format.h>
#include <xdrpp/marshal.h>

namespace cereal
{
template <class Archive>
void
save(Archive& ar, spn::Upgrades::UpgradeParameters const& p)
{
    ar(make_nvp("time", spn::VirtualClock::to_time_t(p.mUpgradeTime)));
    ar(make_nvp("version", p.mProtocolVersion));
    ar(make_nvp("fee", p.mBaseFee));
    ar(make_nvp("maxtxsize", p.mMaxTxSize));
    ar(make_nvp("reserve", p.mBaseReserve));
}

template <class Archive>
void
load(Archive& ar, spn::Upgrades::UpgradeParameters& o)
{
    time_t t;
    ar(make_nvp("time", t));
    o.mUpgradeTime = spn::VirtualClock::from_time_t(t);
    ar(make_nvp("version", o.mProtocolVersion));
    ar(make_nvp("fee", o.mBaseFee));
    ar(make_nvp("maxtxsize", o.mMaxTxSize));
    ar(make_nvp("reserve", o.mBaseReserve));
}
} // namespace cereal

namespace spn
{
std::string
Upgrades::UpgradeParameters::toJson() const
{
    std::ostringstream out;
    {
        cereal::JSONOutputArchive ar(out);
        cereal::save(ar, *this);
    }
    return out.str();
}

void
Upgrades::UpgradeParameters::fromJson(std::string const& s)
{
    std::istringstream in(s);
    {
        cereal::JSONInputArchive ar(in);
        cereal::load(ar, *this);
    }
}

Upgrades::Upgrades(UpgradeParameters const& params) : mParams(params)
{
}

void
Upgrades::setParameters(UpgradeParameters const& params, Config const& cfg)
{
    if (params.mProtocolVersion &&
        *params.mProtocolVersion > cfg.LEDGER_PROTOCOL_VERSION)
    {
        throw std::invalid_argument(fmt::format(
            "Protocol version error: supported is up to {}, passed is {}",
            cfg.LEDGER_PROTOCOL_VERSION, *params.mProtocolVersion));
    }
    mParams = params;
}

Upgrades::UpgradeParameters const&
Upgrades::getParameters() const
{
    return mParams;
}

std::vector<LedgerUpgrade>
Upgrades::createUpgradesFor(LedgerHeader const& header) const
{
    auto result = std::vector<LedgerUpgrade>{};
    if (!timeForUpgrade(header.scpValue.closeTime))
    {
        return result;
    }

    if (mParams.mProtocolVersion &&
        (header.ledgerVersion != *mParams.mProtocolVersion))
    {
        result.emplace_back(LEDGER_UPGRADE_VERSION);
        result.back().newLedgerVersion() = *mParams.mProtocolVersion;
    }
    if (mParams.mBaseFee && (header.baseFee != *mParams.mBaseFee))
    {
        result.emplace_back(LEDGER_UPGRADE_BASE_FEE);
        result.back().newBaseFee() = *mParams.mBaseFee;
    }
    if (mParams.mMaxTxSize && (header.maxTxSetSize != *mParams.mMaxTxSize))
    {
        result.emplace_back(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
        result.back().newMaxTxSetSize() = *mParams.mMaxTxSize;
    }
    if (mParams.mBaseReserve && (header.baseReserve != *mParams.mBaseReserve))
    {
        result.emplace_back(LEDGER_UPGRADE_BASE_RESERVE);
        result.back().newBaseReserve() = *mParams.mBaseReserve;
    }

    return result;
}

void
Upgrades::applyTo(LedgerUpgrade const& upgrade, AbstractLedgerState& ls)
{
    switch (upgrade.type())
    {
    case LEDGER_UPGRADE_VERSION:
        applyVersionUpgrade(ls, upgrade.newLedgerVersion());
        break;
    case LEDGER_UPGRADE_BASE_FEE:
        ls.loadHeader().current().baseFee = upgrade.newBaseFee();
        break;
    case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
        ls.loadHeader().current().maxTxSetSize = upgrade.newMaxTxSetSize();
        break;
    case LEDGER_UPGRADE_BASE_RESERVE:
        applyReserveUpgrade(ls, upgrade.newBaseReserve());
        break;
    default:
    {
        auto s = fmt::format("Unknown upgrade type: {0}", upgrade.type());
        throw std::runtime_error(s);
    }
    }
}

std::string
Upgrades::toString(LedgerUpgrade const& upgrade)
{
    switch (upgrade.type())
    {
    case LEDGER_UPGRADE_VERSION:
        return fmt::format("protocolversion={0}", upgrade.newLedgerVersion());
    case LEDGER_UPGRADE_BASE_FEE:
        return fmt::format("basefee={0}", upgrade.newBaseFee());
    case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
        return fmt::format("maxtxsetsize={0}", upgrade.newMaxTxSetSize());
    case LEDGER_UPGRADE_BASE_RESERVE:
        return fmt::format("basereserve={0}", upgrade.newBaseReserve());
    default:
        return "<unsupported>";
    }
}

std::string
Upgrades::toString() const
{
    fmt::MemoryWriter r;

    auto appendInfo = [&](std::string const& s, optional<uint32> const& o) {
        if (o)
        {
            if (!r.size())
            {
                r << "upgradetime="
                  << VirtualClock::pointToISOString(mParams.mUpgradeTime);
            }
            r << ", " << s << "=" << *o;
        }
    };
    appendInfo("protocolversion", mParams.mProtocolVersion);
    appendInfo("basefee", mParams.mBaseFee);
    appendInfo("basereserve", mParams.mBaseReserve);
    appendInfo("maxtxsize", mParams.mMaxTxSize);

    return r.str();
}

Upgrades::UpgradeParameters
Upgrades::removeUpgrades(std::vector<UpgradeType>::const_iterator beginUpdates,
                         std::vector<UpgradeType>::const_iterator endUpdates,
                         bool& updated)
{
    updated = false;
    UpgradeParameters res = mParams;

    auto resetParam = [&](optional<uint32>& o, uint32 v) {
        if (o && *o == v)
        {
            o.reset();
            updated = true;
        }
    };

    for (auto it = beginUpdates; it != endUpdates; it++)
    {
        auto& u = *it;
        LedgerUpgrade lu;
        try
        {
            xdr::xdr_from_opaque(u, lu);
        }
        catch (xdr::xdr_runtime_error&)
        {
            continue;
        }
        switch (lu.type())
        {
        case LEDGER_UPGRADE_VERSION:
            resetParam(res.mProtocolVersion, lu.newLedgerVersion());
            break;
        case LEDGER_UPGRADE_BASE_FEE:
            resetParam(res.mBaseFee, lu.newBaseFee());
            break;
        case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
            resetParam(res.mMaxTxSize, lu.newMaxTxSetSize());
            break;
        case LEDGER_UPGRADE_BASE_RESERVE:
            resetParam(res.mBaseReserve, lu.newBaseReserve());
            break;
        default:
            // skip unknown
            break;
        }
    }
    return res;
}

bool
Upgrades::isValid(UpgradeType const& upgrade, LedgerUpgradeType& upgradeType,
                  bool nomination, Config const& cfg,
                  LedgerHeader const& header) const
{
    if (nomination && !timeForUpgrade(header.scpValue.closeTime))
    {
        return false;
    }

    LedgerUpgrade lupgrade;

    try
    {
        xdr::xdr_from_opaque(upgrade, lupgrade);
    }
    catch (xdr::xdr_runtime_error&)
    {
        return false;
    }

    bool res = true;
    switch (lupgrade.type())
    {
    case LEDGER_UPGRADE_VERSION:
    {
        uint32 newVersion = lupgrade.newLedgerVersion();
        if (nomination)
        {
            res = mParams.mProtocolVersion &&
                  (newVersion == *mParams.mProtocolVersion);
        }
        // only allow upgrades to a supported version of the protocol
        res = res && (newVersion <= cfg.LEDGER_PROTOCOL_VERSION);
        // and enforce versions to be strictly monotonic
        res = res && (newVersion > header.ledgerVersion);
    }
    break;
    case LEDGER_UPGRADE_BASE_FEE:
    {
        uint32 newFee = lupgrade.newBaseFee();
        if (nomination)
        {
            res = mParams.mBaseFee && (newFee == *mParams.mBaseFee);
        }
        res = res && (newFee != 0);
    }
    break;
    case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
    {
        uint32 newMax = lupgrade.newMaxTxSetSize();
        if (nomination)
        {
            res = mParams.mMaxTxSize && (newMax == *mParams.mMaxTxSize);
        }
        res = res && (newMax != 0);
    }
    break;
    case LEDGER_UPGRADE_BASE_RESERVE:
    {
        uint32 newReserve = lupgrade.newBaseReserve();
        if (nomination)
        {
            res = mParams.mBaseReserve && (newReserve == *mParams.mBaseReserve);
        }
        res = res && (newReserve != 0);
    }
    break;
    default:
        res = false;
    }
    if (res)
    {
        upgradeType = lupgrade.type();
    }
    return res;
}

bool
Upgrades::timeForUpgrade(uint64_t time) const
{
    return mParams.mUpgradeTime <= VirtualClock::from_time_t(time);
}

void
Upgrades::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS upgradehistory";
    db.getSession() << "CREATE TABLE upgradehistory ("
                       "ledgerseq    INT NOT NULL CHECK (ledgerseq >= 0), "
                       "upgradeindex INT NOT NULL, "
                       "upgrade      TEXT NOT NULL, "
                       "changes      TEXT NOT NULL, "
                       "PRIMARY KEY (ledgerseq, upgradeindex)"
                       ")";
    db.getSession()
        << "CREATE INDEX upgradehistbyseq ON upgradehistory (ledgerseq);";
}

void
Upgrades::storeUpgradeHistory(Database& db, uint32_t ledgerSeq,
                              LedgerUpgrade const& upgrade,
                              LedgerEntryChanges const& changes, int index)
{
    xdr::opaque_vec<> upgradeContent(xdr::xdr_to_opaque(upgrade));
    std::string upgradeContent64 = decoder::encode_b64(upgradeContent);

    xdr::opaque_vec<> upgradeChanges(xdr::xdr_to_opaque(changes));
    std::string upgradeChanges64 = decoder::encode_b64(upgradeChanges);

    auto prep = db.getPreparedStatement(
        "INSERT INTO upgradehistory "
        "(ledgerseq, upgradeindex,  upgrade,  changes) VALUES "
        "(:seq,      :upgradeindex, :upgrade, :changes)");

    auto& st = prep.statement();
    st.exchange(soci::use(ledgerSeq));
    st.exchange(soci::use(index));
    st.exchange(soci::use(upgradeContent64));
    st.exchange(soci::use(upgradeChanges64));
    st.define_and_bind();
    {
        auto timer = db.getInsertTimer("upgradehistory");
        st.execute(true);
    }

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}

void
Upgrades::deleteOldEntries(Database& db, uint32_t ledgerSeq, uint32_t count)
{
    DatabaseUtils::deleteOldEntriesHelper(db.getSession(), ledgerSeq, count,
                                          "upgradehistory", "ledgerseq");
}

static void
addLiabilities(std::map<Asset, std::unique_ptr<int64_t>>& liabilities,
               AccountID const& accountID, Asset const& asset, int64_t delta)
{
    auto iter =
        liabilities.insert(std::make_pair(asset, std::make_unique<int64_t>(0)))
            .first;
    if (asset.type() != ASSET_TYPE_NATIVE && accountID == getIssuer(asset))
    {
        return;
    }
    if (iter->second)
    {
        if (!spn::addBalance(*iter->second, delta))
        {
            iter->second.reset();
        }
    }
}

static int64_t
getAvailableBalanceExcludingLiabilities(AccountID const& accountID,
                                        Asset const& asset,
                                        int64_t balanceAboveReserve,
                                        AbstractLedgerState& ls)
{
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        return balanceAboveReserve;
    }

    if (accountID == getIssuer(asset))
    {
        return INT64_MAX;
    }
    else
    {
        auto trust = spn::loadTrustLineWithoutRecord(ls, accountID, asset);
        if (trust && trust.isAuthorized())
        {
            return trust.getBalance();
        }
        else
        {
            return 0;
        }
    }
}

static int64_t
getAvailableLimitExcludingLiabilities(AccountID const& accountID,
                                      Asset const& asset, int64_t balance,
                                      AbstractLedgerState& ls)
{
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        return INT64_MAX - balance;
    }

    if (accountID == getIssuer(asset))
    {
        return INT64_MAX;
    }
    else
    {
        LedgerKey key(TRUSTLINE);
        key.trustLine().accountID = accountID;
        key.trustLine().asset = asset;
        auto trust = ls.loadWithoutRecord(key);
        if (trust && isAuthorized(trust))
        {
            auto const& tl = trust.current().data.trustLine();
            return tl.limit - tl.balance;
        }
        else
        {
            return 0;
        }
    }
}

static bool
shouldDeleteOffer(Asset const& asset, int64_t effectiveBalance,
                  std::map<Asset, std::unique_ptr<int64_t>> const& liabilities,
                  std::function<int64_t(Asset const&, int64_t)> getCap)
{
    auto iter = liabilities.find(asset);
    if (iter == liabilities.end())
    {
        throw std::runtime_error("liabilities were not calculated");
    }
    // Offers should be deleted if liabilities exceed INT64_MAX (nullptr) or if
    // there are excess liabilities.
    return iter->second ? *iter->second > getCap(asset, effectiveBalance)
                        : true;
}

enum class UpdateOfferResult
{
    Unchanged,
    Adjusted,
    AdjustedToZero,
    Erased
};

static UpdateOfferResult
updateOffer(
    LedgerStateEntry& offerEntry, int64_t balance, int64_t balanceAboveReserve,
    std::map<Asset, Liabilities>& liabilities,
    std::map<Asset, std::unique_ptr<int64_t>> const& initialBuyingLiabilities,
    std::map<Asset, std::unique_ptr<int64_t>> const& initialSellingLiabilities,
    AbstractLedgerState& ls, LedgerStateHeader const& header)
{
    using namespace std::placeholders;
    auto& offer = offerEntry.current().data.offer();

    auto availableBalanceBind =
        std::bind(getAvailableBalanceExcludingLiabilities, offer.sellerID, _1,
                  _2, std::ref(ls));
    auto availableLimitBind = std::bind(getAvailableLimitExcludingLiabilities,
                                        offer.sellerID, _1, _2, std::ref(ls));

    bool erase =
        shouldDeleteOffer(offer.selling, balanceAboveReserve,
                          initialSellingLiabilities, availableBalanceBind);
    erase = erase ||
            shouldDeleteOffer(offer.buying, balance, initialBuyingLiabilities,
                              availableLimitBind);
    UpdateOfferResult res =
        erase ? UpdateOfferResult::Erased : UpdateOfferResult::Unchanged;

    // If erase == false then we know that the total buying liabilities
    // of the buying asset do not exceed its available limit, and the
    // total selling liabilities of the selling asset do not exceed its
    // available balance. This implies that there are no excess
    // liabilities for this offer, so the only applicable limit is the
    // offer amount. We then use adjustOffer to check that it will
    // satisfy thresholds.
    if (!erase && adjustOffer(offer.price, offer.amount, INT64_MAX) == 0)
    {
        erase = true;
        res = UpdateOfferResult::AdjustedToZero;
    }

    if (erase)
    {
        offerEntry.erase();
    }
    else
    {
        // The same logic for adjustOffer discussed above applies here,
        // except that we now actually update the offer to reflect the
        // adjustment.
        auto adjAmount = adjustOffer(offer.price, offer.amount, INT64_MAX);
        if (adjAmount != offer.amount)
        {
            offer.amount = adjAmount;
            res = UpdateOfferResult::Adjusted;
        }

        if (offer.buying.type() == ASSET_TYPE_NATIVE ||
            !(offer.sellerID == getIssuer(offer.buying)))
        {
            if (!spn::addBalance(
                    liabilities[offer.buying].buying,
                    getOfferBuyingLiabilities(header, offerEntry)))
            {
                throw std::runtime_error("could not add buying "
                                         "liabilities");
            }
        }
        if (offer.selling.type() == ASSET_TYPE_NATIVE ||
            !(offer.sellerID == getIssuer(offer.selling)))
        {
            if (!spn::addBalance(
                    liabilities[offer.selling].selling,
                    getOfferSellingLiabilities(header, offerEntry)))
            {
                throw std::runtime_error("could not add selling "
                                         "liabilities");
            }
        }
    }
    return res;
}

// This function is used to bring offers and liabilities into a valid state.
// For every account that has offers,
//   1. Calculate total liabilities for each asset
//   2. For every asset with excess buying liabilities according to (1), erase
//      all offers buying that asset. For every asset with excess selling
//      liabilities according to (1), erase all offers selling that asset.
//   3. Update liabilities to reflect offers remaining in the book.
// It is essential to note that the excess liabilities are determined only
// using the initial result of step (1), so it does not matter what order the
// offers are processed.
static void
prepareLiabilities(AbstractLedgerState& ls, LedgerStateHeader const& header)
{
    CLOG(INFO, "Ledger") << "Starting prepareLiabilities";

    auto offersByAccount = ls.loadAllOffers();

    uint64_t nChangedAccounts = 0;
    uint64_t nChangedTrustLines = 0;
    std::map<UpdateOfferResult, uint64_t> nUpdatedOffers;
    for (auto& accountOffers : offersByAccount)
    {
        // The purpose of std::unique_ptr here is to have a special value
        // (nullptr) to indicate that an integer overflow would have occured.
        // Overflow is possible here because existing offers were not
        // constrainted to have int64_t liabilities. This must be carefully
        // handled in what follows.
        std::map<Asset, std::unique_ptr<int64_t>> initialBuyingLiabilities;
        std::map<Asset, std::unique_ptr<int64_t>> initialSellingLiabilities;
        for (auto const& offerEntry : accountOffers.second)
        {
            auto const& offer = offerEntry.current().data.offer();
            addLiabilities(initialBuyingLiabilities, offer.sellerID,
                           offer.buying,
                           getOfferBuyingLiabilities(header, offerEntry));
            addLiabilities(initialSellingLiabilities, offer.sellerID,
                           offer.selling,
                           getOfferSellingLiabilities(header, offerEntry));
        }

        auto accountEntry = spn::loadAccount(ls, accountOffers.first);
        if (!accountEntry)
        {
            throw std::runtime_error("account does not exist");
        }
        auto const& acc = accountEntry.current().data.account();
        AccountEntry const accountBefore = acc;

        // balanceAboveReserve must exclude native selling liabilities, since
        // these are in the process of being recalculated from scratch.
        int64_t balance = acc.balance;
        int64_t minBalance = getMinBalance(header, acc.numSubEntries);
        int64_t balanceAboveReserve = balance - minBalance;

        std::map<Asset, Liabilities> liabilities;
        for (auto& offerEntry : accountOffers.second)
        {
            auto offerID = offerEntry.current().data.offer().offerID;
            auto res = updateOffer(offerEntry, balance, balanceAboveReserve,
                                   liabilities, initialBuyingLiabilities,
                                   initialSellingLiabilities, ls, header);
            if (res == UpdateOfferResult::AdjustedToZero ||
                res == UpdateOfferResult::Erased)
            {
                spn::addNumEntries(header, accountEntry, -1);
            }

            ++nUpdatedOffers[res];
            if (res != UpdateOfferResult::Unchanged)
            {
                std::string message;
                switch (res)
                {
                case UpdateOfferResult::Adjusted:
                    message = " was adjusted";
                    break;
                case UpdateOfferResult::AdjustedToZero:
                    message = " was adjusted to zero";
                    break;
                case UpdateOfferResult::Erased:
                    message = " was erased";
                    break;
                default:
                    throw std::runtime_error("Unknown UpdateOfferResult");
                }
                CLOG(DEBUG, "Ledger")
                    << "Offer with offerID=" << offerID << message;
            }
        }

        for (auto const& assetLiabilities : liabilities)
        {
            Asset const& asset = assetLiabilities.first;
            Liabilities const& liab = assetLiabilities.second;
            if (asset.type() == ASSET_TYPE_NATIVE)
            {
                int64_t deltaSelling =
                    liab.selling - getSellingLiabilities(header, accountEntry);
                int64_t deltaBuying =
                    liab.buying - getBuyingLiabilities(header, accountEntry);
                if (!addSellingLiabilities(header, accountEntry, deltaSelling))
                {
                    throw std::runtime_error("invalid selling liabilities "
                                             "during upgrade");
                }
                if (!addBuyingLiabilities(header, accountEntry, deltaBuying))
                {
                    throw std::runtime_error("invalid buying liabilities "
                                             "during upgrade");
                }
            }
            else
            {
                auto trustEntry =
                    spn::loadTrustLine(ls, accountOffers.first, asset);
                int64_t deltaSelling =
                    liab.selling - trustEntry.getSellingLiabilities(header);
                int64_t deltaBuying =
                    liab.buying - trustEntry.getBuyingLiabilities(header);
                if (deltaSelling != 0 || deltaBuying != 0)
                {
                    ++nChangedTrustLines;
                }

                if (!trustEntry.addSellingLiabilities(header, deltaSelling))
                {
                    throw std::runtime_error("invalid selling liabilities "
                                             "during upgrade");
                }
                if (!trustEntry.addBuyingLiabilities(header, deltaBuying))
                {
                    throw std::runtime_error("invalid buying liabilities "
                                             "during upgrade");
                }
            }
        }

        if (!(acc == accountBefore))
        {
            ++nChangedAccounts;
        }
    }

    CLOG(INFO, "Ledger") << "prepareLiabilities completed with "
                         << nChangedAccounts << " accounts modified, "
                         << nChangedTrustLines << " trustlines modified, "
                         << nUpdatedOffers[UpdateOfferResult::Adjusted]
                         << " offers adjusted, "
                         << nUpdatedOffers[UpdateOfferResult::AdjustedToZero]
                         << " offers adjusted to zero, and "
                         << nUpdatedOffers[UpdateOfferResult::Erased]
                         << " offers erased";
}

void
Upgrades::applyVersionUpgrade(AbstractLedgerState& ls, uint32_t newVersion)
{
    auto header = ls.loadHeader();
    uint32_t prevVersion = header.current().ledgerVersion;

    header.current().ledgerVersion = newVersion;
    if (header.current().ledgerVersion >= 10 && prevVersion < 10)
    {
        prepareLiabilities(ls, header);
    }
}

void
Upgrades::applyReserveUpgrade(AbstractLedgerState& ls, uint32_t newReserve)
{
    auto header = ls.loadHeader();
    bool didReserveIncrease = newReserve > header.current().baseReserve;

    header.current().baseReserve = newReserve;
    if (header.current().ledgerVersion >= 10 && didReserveIncrease)
    {
        prepareLiabilities(ls, header);
    }
}
}
