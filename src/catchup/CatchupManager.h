#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupWork.h"
#include <functional>
#include <memory>
#include <system_error>

namespace asio
{

typedef std::error_code error_code;
};

namespace spn
{

class Application;

class CatchupManager
{
  public:
    static std::unique_ptr<CatchupManager> create(Application& app);

    // Callback from catchup, indicates that any catchup work is done.
    virtual void historyCaughtup() = 0;

    // Run catchup with given configuration and verify mode.
    virtual void catchupHistory(CatchupConfiguration catchupConfiguration,
                                bool manualCatchup,
                                CatchupWork::ProgressHandler handler) = 0;

    // Return status of catchup for or empty string, if no catchup in progress
    virtual std::string getStatus() const = 0;

    // Emit a log message and set StatusManager HISTORY_CATCHUP status to
    // describe current catchup state. The `contiguous` argument is passed in
    // to describe whether the ledger-manager's view of current catchup tasks
    // is currently contiguous or discontiguous. Message is passed as second
    // argument.
    virtual void logAndUpdateCatchupStatus(bool contiguous,
                                           std::string const& message) = 0;

    // Emit a log message and set StatusManager HISTORY_CATCHUP status to
    // describe current catchup state. The `contiguous` argument is passed in
    // to describe whether the ledger-manager's view of current catchup tasks
    // is currently contiguous or discontiguous. Message is taken from current
    // work item.
    virtual void logAndUpdateCatchupStatus(bool contiguous) = 0;

    virtual ~CatchupManager(){};
};
}
