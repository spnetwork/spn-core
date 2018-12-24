// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/TestAccount.h"
#include "ledger/LedgerState.h"
#include "ledger/LedgerStateEntry.h"
#include "ledger/LedgerStateHeader.h"
#include "main/Application.h"
#include "test/TestExceptions.h"
#include "test/TxTests.h"
#include "transactions/TransactionUtils.h"

#include <lib/catch.hpp>

namespace spn
{

using namespace txtest;

SequenceNumber
TestAccount::loadSequenceNumber()
{
    mSn = 0;
    return getLastSequenceNumber();
}

void
TestAccount::updateSequenceNumber()
{
    if (mSn == 0)
    {
        LedgerState ls(mApp.getLedgerStateRoot());
        auto entry = spn::loadAccount(ls, getPublicKey());
        if (entry)
        {
            mSn = entry.current().data.account().seqNum;
        }
    }
}

int64_t
TestAccount::getBalance() const
{
    LedgerState ls(mApp.getLedgerStateRoot());
    auto entry = spn::loadAccount(ls, getPublicKey());
    return entry.current().data.account().balance;
}

bool
TestAccount::exists() const
{
    return doesAccountExist(mApp, getPublicKey());
}

TransactionFramePtr
TestAccount::tx(std::vector<Operation> const& ops, SequenceNumber sn)
{
    if (sn == 0)
    {
        sn = nextSequenceNumber();
    }

    return transactionFromOperations(mApp, getSecretKey(), sn, ops);
}

Operation
TestAccount::op(Operation operation)
{
    operation.sourceAccount.activate() = getPublicKey();
    return operation;
}

TestAccount
TestAccount::createRoot(Application& app)
{
    auto secretKey = getRoot(app.getNetworkID());
    return TestAccount{app, secretKey};
}

TestAccount
TestAccount::create(SecretKey const& secretKey, uint64_t initialBalance)
{
    auto publicKey = secretKey.getPublicKey();

    std::unique_ptr<LedgerEntry> destBefore;
    {
        LedgerState ls(mApp.getLedgerStateRoot());
        auto entry = spn::loadAccount(ls, publicKey);
        if (entry)
        {
            destBefore = std::make_unique<LedgerEntry>(entry.current());
        }
    }

    try
    {
        applyTx(tx({createAccount(publicKey, initialBalance)}), mApp);
    }
    catch (...)
    {
        LedgerState ls(mApp.getLedgerStateRoot());
        auto destAfter = spn::loadAccount(ls, publicKey);
        // check that the target account didn't change
        REQUIRE(!!destBefore == !!destAfter);
        if (destBefore && destAfter)
        {
            REQUIRE(*destBefore == destAfter.current());
        }
        throw;
    }

    {
        LedgerState ls(mApp.getLedgerStateRoot());
        REQUIRE(spn::loadAccount(ls, publicKey));
    }
    return TestAccount{mApp, secretKey};
}

TestAccount
TestAccount::create(std::string const& name, uint64_t initialBalance)
{
    return create(getAccount(name.c_str()), initialBalance);
}

void
TestAccount::merge(PublicKey const& into)
{
    applyTx(tx({accountMerge(into)}), mApp);

    LedgerState ls(mApp.getLedgerStateRoot());
    REQUIRE(spn::loadAccount(ls, into));
    REQUIRE(!spn::loadAccount(ls, getPublicKey()));
}

void
TestAccount::inflation()
{
    applyTx(tx({txtest::inflation()}), mApp);
}

Asset
TestAccount::asset(std::string const& name)
{
    return txtest::makeAsset(*this, name);
}

void
TestAccount::changeTrust(Asset const& asset, int64_t limit)
{
    applyTx(tx({txtest::changeTrust(asset, limit)}), mApp);
}

void
TestAccount::allowTrust(Asset const& asset, PublicKey const& trustor)
{
    applyTx(tx({txtest::allowTrust(trustor, asset, true)}), mApp);
}

void
TestAccount::denyTrust(Asset const& asset, PublicKey const& trustor)
{
    applyTx(tx({txtest::allowTrust(trustor, asset, false)}), mApp);
}

TrustLineEntry
TestAccount::loadTrustLine(Asset const& asset) const
{
    LedgerState ls(mApp.getLedgerStateRoot());
    LedgerKey key(TRUSTLINE);
    key.trustLine().accountID = getPublicKey();
    key.trustLine().asset = asset;
    return ls.load(key).current().data.trustLine();
}

bool
TestAccount::hasTrustLine(Asset const& asset) const
{
    LedgerState ls(mApp.getLedgerStateRoot());
    LedgerKey key(TRUSTLINE);
    key.trustLine().accountID = getPublicKey();
    key.trustLine().asset = asset;
    return (bool)ls.load(key);
}

void
TestAccount::setOptions(SetOptionsArguments const& arguments)
{
    applyTx(tx({txtest::setOptions(arguments)}), mApp);
}

void
TestAccount::manageData(std::string const& name, DataValue* value)
{
    applyTx(tx({txtest::manageData(name, value)}), mApp);

    LedgerState ls(mApp.getLedgerStateRoot());
    auto data = spn::loadData(ls, getPublicKey(), name);
    if (value)
    {
        REQUIRE(data);
        REQUIRE(data.current().data.data().dataValue == *value);
    }
    else
    {
        REQUIRE(!data);
    }
}

void
TestAccount::bumpSequence(SequenceNumber to)
{
    applyTx(tx({txtest::bumpSequence(to)}), mApp, false);
}

uint64_t
TestAccount::manageOffer(uint64_t offerID, Asset const& selling,
                         Asset const& buying, Price const& price,
                         int64_t amount, ManageOfferEffect expectedEffect)
{
    return applyManageOffer(mApp, offerID, getSecretKey(), selling, buying,
                            price, amount, nextSequenceNumber(),
                            expectedEffect);
}

uint64_t
TestAccount::createPassiveOffer(Asset const& selling, Asset const& buying,
                                Price const& price, int64_t amount,
                                ManageOfferEffect expectedEffect)
{
    return applyCreatePassiveOffer(mApp, getSecretKey(), selling, buying, price,
                                   amount, nextSequenceNumber(),
                                   expectedEffect);
}

void
TestAccount::pay(PublicKey const& destination, int64_t amount)
{
    std::unique_ptr<LedgerEntry> toAccount;
    {
        LedgerState ls(mApp.getLedgerStateRoot());
        auto toAccountEntry = spn::loadAccount(ls, destination);
        toAccount =
            toAccountEntry
                ? std::make_unique<LedgerEntry>(toAccountEntry.current())
                : nullptr;
        if (destination == getPublicKey())
        {
            REQUIRE(toAccountEntry);
        }
        else
        {
            REQUIRE(spn::loadAccount(ls, getPublicKey()));
        }
    }

    auto transaction = tx({payment(destination, amount)});

    try
    {
        applyTx(transaction, mApp);
    }
    catch (...)
    {
        LedgerState ls(mApp.getLedgerStateRoot());
        auto toAccountAfter = spn::loadAccount(ls, destination);
        // check that the target account didn't change
        REQUIRE(!!toAccount == !!toAccountAfter);
        if (toAccount && toAccountAfter &&
            !(toAccount->data.account().accountID ==
              toAccountAfter.current().data.account().accountID))
        {
            REQUIRE(*toAccount == toAccountAfter.current());
        }
        throw;
    }

    LedgerState ls(mApp.getLedgerStateRoot());
    auto toAccountAfter = spn::loadAccount(ls, destination);
    REQUIRE(toAccount);
    REQUIRE(toAccountAfter);
}

void
TestAccount::pay(PublicKey const& destination, Asset const& asset,
                 int64_t amount)
{
    applyTx(tx({payment(destination, asset, amount)}), mApp);
}

PathPaymentResult
TestAccount::pay(PublicKey const& destination, Asset const& sendCur,
                 int64_t sendMax, Asset const& destCur, int64_t destAmount,
                 std::vector<Asset> const& path, Asset* noIssuer)
{
    auto transaction = tx({pathPayment(destination, sendCur, sendMax, destCur,
                                       destAmount, path)});
    try
    {
        applyTx(transaction, mApp);
    }
    catch (ex_PATH_PAYMENT_NO_ISSUER&)
    {
        REQUIRE(noIssuer);
        REQUIRE(*noIssuer == transaction->getResult()
                                 .result.results()[0]
                                 .tr()
                                 .pathPaymentResult()
                                 .noIssuer());
        throw;
    }

    REQUIRE(!noIssuer);

    return getFirstResult(*transaction).tr().pathPaymentResult();
}
};
