#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-ledger.h"

#include "main/Config.h"
#include "util/Timer.h"
#include "util/optional.h"
#include <stdint.h>
#include <vector>

namespace spn
{
class AbstractLedgerState;
class Config;
class Database;
struct LedgerHeader;
struct LedgerUpgrade;

class Upgrades
{
  public:
    struct UpgradeParameters
    {
        UpgradeParameters()
        {
        }
        UpgradeParameters(Config const& cfg)
        {
            mUpgradeTime = cfg.TESTING_UPGRADE_DATETIME;
            mProtocolVersion =
                make_optional<uint32>(cfg.LEDGER_PROTOCOL_VERSION);
            mBaseFee = make_optional<uint32>(cfg.TESTING_UPGRADE_DESIRED_FEE);
            mMaxTxSize =
                make_optional<uint32>(cfg.TESTING_UPGRADE_MAX_TX_PER_LEDGER);
            mBaseReserve = make_optional<uint32>(cfg.TESTING_UPGRADE_RESERVE);
        }
        VirtualClock::time_point mUpgradeTime;
        optional<uint32> mProtocolVersion;
        optional<uint32> mBaseFee;
        optional<uint32> mMaxTxSize;
        optional<uint32> mBaseReserve;

        std::string toJson() const;
        void fromJson(std::string const& s);
    };

    Upgrades()
    {
    }
    explicit Upgrades(UpgradeParameters const& params);

    void setParameters(UpgradeParameters const& params, Config const& cfg);

    UpgradeParameters const& getParameters() const;

    // create upgrades for given ledger
    std::vector<LedgerUpgrade>
    createUpgradesFor(LedgerHeader const& header) const;

    // apply upgrade to ledger header
    static void applyTo(LedgerUpgrade const& upgrade, AbstractLedgerState& ls);

    // convert upgrade value to string
    static std::string toString(LedgerUpgrade const& upgrade);

    // returns true if upgrade is a valid upgrade step
    // in which case it also sets upgradeType
    bool isValid(UpgradeType const& upgrade, LedgerUpgradeType& upgradeType,
                 bool nomination, Config const& cfg,
                 LedgerHeader const& header) const;

    // constructs a human readable string that represents
    // the pending upgrades
    std::string toString() const;

    // sets updated to true if some upgrades were removed
    UpgradeParameters
    removeUpgrades(std::vector<UpgradeType>::const_iterator beginUpdates,
                   std::vector<UpgradeType>::const_iterator endUpdates,
                   bool& updated);

    static void dropAll(Database& db);

    static void storeUpgradeHistory(Database& db, uint32_t ledgerSeq,
                                    LedgerUpgrade const& upgrade,
                                    LedgerEntryChanges const& changes,
                                    int index);
    static void deleteOldEntries(Database& db, uint32_t ledgerSeq,
                                 uint32_t count);

  private:
    UpgradeParameters mParams;

    bool timeForUpgrade(uint64_t time) const;

    static void applyVersionUpgrade(AbstractLedgerState& ls,
                                    uint32_t newVersion);

    static void applyReserveUpgrade(AbstractLedgerState& ls,
                                    uint32_t newReserve);
};
}
