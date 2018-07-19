#ifndef _HOTSTUFF_TYPE_H
#define _HOTSTUFF_TYPE_H

#include "promise.hpp"
#include "salticidae/event.h"
#include "salticidae/ref.h"
#include "salticidae/netaddr.h"
#include "salticidae/stream.h"
#include "salticidae/type.h"
#include "salticidae/util.h"

namespace hotstuff {

using salticidae::RcObj;
using salticidae::BoxObj;

using salticidae::uint256_t;
using salticidae::DataStream;
using salticidae::htole;
using salticidae::letoh;
using salticidae::get_hex;
using salticidae::from_hex;
using salticidae::bytearray_t;
using salticidae::get_hash;

using salticidae::NetAddr;
using salticidae::Event;
using salticidae::EventContext;
using promise::promise_t;

inline std::string get_hex10(const uint256_t &x) {
    return get_hex(x).substr(0, 10);
}

class HotStuffError: public salticidae::SalticidaeError {
    public:
    template<typename... Args>
    HotStuffError(Args... args): salticidae::SalticidaeError(args...) {}
};

class HotStuffInvalidEntity: public HotStuffError {
    public:
    template<typename... Args>
    HotStuffInvalidEntity(Args... args): HotStuffError(args...) {}
};

using salticidae::Serializable;

class Cloneable {
    public:
    virtual ~Cloneable() = default;
    virtual Cloneable *clone() = 0;
};

using ReplicaID = uint16_t;

}

#endif