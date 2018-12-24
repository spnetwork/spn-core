// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/CommandLine.h"
#include "main/DeprecatedCommandLine.h"
#include "util/Logging.h"

#include <sodium/core.h>
#include <xdrpp/marshal.h>

INITIALIZE_EASYLOGGINGPP

int
main(int argc, char* const* argv)
{
    using namespace spn;

    Logging::init();
    if (sodium_init() != 0)
    {
        LOG(FATAL) << "Could not initialize crypto";
        return 1;
    }
    xdr::marshaling_stack_limit = 1000;

    auto result = handleCommandLine(argc, argv);
    if (result)
    {
        return *result;
    }

    return handleDeprecatedCommandLine(argc, argv);
}
