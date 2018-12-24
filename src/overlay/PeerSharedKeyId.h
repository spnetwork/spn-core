#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ECDH.h"
#include "overlay/Peer.h"

namespace spn
{
struct PeerSharedKeyId
{
    Curve25519Public mECDHPublicKey;
    Peer::PeerRole mRole;

    friend bool operator==(PeerSharedKeyId const& x, PeerSharedKeyId const& y);
    friend bool operator!=(PeerSharedKeyId const& x, PeerSharedKeyId const& y);
};
}

namespace std
{
template <> struct hash<spn::PeerSharedKeyId>
{
    size_t operator()(spn::PeerSharedKeyId const& x) const noexcept;
};
}
