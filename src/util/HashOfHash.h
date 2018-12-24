#pragma once
#include <xdr/Stellar-types.h>

namespace std
{
template <> struct hash<spn::uint256>
{
    size_t operator()(spn::uint256 const& x) const noexcept;
};
}
