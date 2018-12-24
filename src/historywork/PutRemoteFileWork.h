// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "historywork/RunCommandWork.h"

namespace spn
{

class HistoryArchive;

class PutRemoteFileWork : public RunCommandWork
{
    std::string mRemote;
    std::string mLocal;
    std::shared_ptr<HistoryArchive> mArchive;
    void getCommand(std::string& cmdLine, std::string& outFile) override;

  public:
    PutRemoteFileWork(Application& app, WorkParent& parent,
                      std::string const& remote, std::string const& local,
                      std::shared_ptr<HistoryArchive> archive);
    ~PutRemoteFileWork();

    Work::State onSuccess() override;
    void onFailureRaise() override;
};
}
