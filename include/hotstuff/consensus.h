/**
 * Copyright 2018 VMware
 * Copyright 2018 Ted Yin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _HOTSTUFF_CONSENSUS_H
#define _HOTSTUFF_CONSENSUS_H

#include <cassert>
#include <set>
#include <unordered_map>

#include "hotstuff/promise.hpp"
#include "hotstuff/type.h"
#include "hotstuff/entity.h"
#include "hotstuff/crypto.h"

namespace hotstuff {

struct Proposal;
struct Vote;
struct Notify;
struct Blame;
struct BlameNotify;
struct Finality;

// TODO: optimize the repr of Status
using status_cert_t = salticidae::BoxObj<std::vector<Notify>>;

/** Abstraction for HotStuff protocol state machine (without network implementation). */
class HotStuffCore {
    block_t b0;                                  /** the genesis block */
    /* === state variables === */
    /** block containing the QC for the highest block having one */
    block_t bqc;
    block_t bexec;                            /**< last executed block */
    uint32_t vheight;          /**< height of the block last voted for */
    uint32_t nheight;          /**< height of the block last notified for */
    uint32_t view;             /**< the current view number */
    /* Q: does the proposer retry the same block in a new view? */
    /* Q: vheight & nheight & gaps? */
    status_cert_t status_cert; /**< status certificate */

    /* === auxilliary variables === */
    privkey_bt priv_key;            /**< private key for signing votes */
    std::set<block_t, BlockHeightCmp> tails;   /**< set of tail blocks */
    ReplicaConfig config;                   /**< replica configuration */
    /* === async event queues === */
    std::unordered_map<block_t, promise_t> qc_waiting;
    promise_t propose_waiting;
    promise_t receive_proposal_waiting;
    promise_t bqc_update_waiting;
    /* == feature switches == */
    /** always vote negatively, useful for some PaceMakers */
    bool neg_vote;

    block_t get_delivered_blk(const uint256_t &blk_hash);
    void sanity_check_delivered(const block_t &blk);
    void check_commit(const block_t &_bqc);
    bool update(const block_t blk);
    void broadcast_vote(const block_t &blk);
    void on_bqc_update();
    void on_qc_finish(const block_t &blk);
    void on_propose_(const Proposal &prop);
    void on_receive_proposal_(const Proposal &prop);

    protected:
    ReplicaID id;                  /**< identity of the replica itself */

    public:
    BoxObj<EntityStorage> storage;

    HotStuffCore(ReplicaID id, privkey_bt &&priv_key);
    virtual ~HotStuffCore() {}

    /* Inputs of the state machine triggered by external events, should called
     * by the class user, with proper invariants. */

    /** Call to initialize the protocol, should be called once before all other
     * functions. */
    void on_init(uint32_t nfaulty, double delta) {
        config.nmajority = nfaulty + 1;
        config.delta = delta;
    }

    /* TODO: better name for "delivery" ? */
    /** Call to inform the state machine that a block is ready to be handled.
     * A block is only delivered if itself is fetched, the block for the
     * contained qc is fetched and all parents are delivered. The user should
     * always ensure this invariant. The invalid blocks will be dropped by this
     * function.
     * @return true if valid */
    bool on_deliver_blk(const block_t &blk);

    /** Call upon the delivery of a proposal message.
     * The block mentioned in the message should be already delivered. */
    void on_receive_proposal(const Proposal &prop);

    /** Call upon the delivery of a vote message.
     * The block mentioned in the message should be already delivered. */
    void on_receive_vote(const Vote &vote);
    void on_receive_notify(const Notify &notify);
    void on_receive_blame(const Blame &blame);
    void on_receive_blamenotify(const BlameNotify &blame);
    void on_commit_timeout(const block_t &blk);

    /** Call to submit new commands to be decided (executed). "Parents" must
     * contain at least one block, and the first block is the actual parent,
     * while the others are uncles/aunts */
    void on_propose(const std::vector<uint256_t> &cmds,
                    const std::vector<block_t> &parents,
                    bytearray_t &&extra = bytearray_t());

    /* Functions required to construct concrete instances for abstract classes.
     * */

    /* Outputs of the state machine triggering external events.  The virtual
     * functions should be implemented by the user to specify the behavior upon
     * the events. */
    protected:
    /** Called by HotStuffCore upon the decision being made for cmd. */
    virtual void do_decide(Finality &&fin) = 0;
    /** Called by HotStuffCore upon broadcasting a new proposal.
     * The user should send the proposal message to all replicas except for
     * itself. */
    virtual void do_broadcast_proposal(const Proposal &prop) = 0;
    virtual void do_broadcast_vote(const Vote &vote) = 0;
    virtual void do_broadcast_notify(const Notify &notify) = 0;
    virtual void do_broadcast_blame(const Blame &blame) = 0;
    virtual void do_broadcast_blamenotify(const BlameNotify &bn) = 0;
    virtual void set_commit_timer(const block_t &blk, double t_sec) = 0;
    virtual void stop_commit_timer(uint32_t height) = 0;

    /* The user plugs in the detailed instances for those
     * polymorphic data types. */
    public:
    /** Create a partial certificate that proves the vote for a block. */
    virtual part_cert_bt create_part_cert(const PrivKey &priv_key, const uint256_t &blk_hash) = 0;
    /** Create a partial certificate from its seralized form. */
    virtual part_cert_bt parse_part_cert(DataStream &s) = 0;
    /** Create a quorum certificate that proves 2f+1 votes for a block. */
    virtual quorum_cert_bt create_quorum_cert(const uint256_t &blk_hash) = 0;
    /** Create a quorum certificate from its serialized form. */
    virtual quorum_cert_bt parse_quorum_cert(DataStream &s) = 0;
    /** Create a command object from its serialized form. */
    //virtual command_t parse_cmd(DataStream &s) = 0;

    public:
    /** Add a replica to the current configuration. This should only be called
     * before running HotStuffCore protocol. */
    void add_replica(ReplicaID rid, const NetAddr &addr, pubkey_bt &&pub_key);
    /** Try to prune blocks lower than last committed height - staleness. */
    void prune(uint32_t staleness);

    /* PaceMaker can use these functions to monitor the core protocol state
     * transition */
    /** Get a promise resolved when the block gets a QC. */
    promise_t async_qc_finish(const block_t &blk);
    /** Get a promise resolved when a new block is proposed. */
    promise_t async_wait_proposal();
    /** Get a promise resolved when a new proposal is received. */
    promise_t async_wait_receive_proposal();
    /** Get a promise resolved when bqc is updated. */
    promise_t async_bqc_update();

    /* Other useful functions */
    const block_t &get_genesis() { return b0; }
    const block_t &get_bqc() { return bqc; }
    const ReplicaConfig &get_config() { return config; }
    ReplicaID get_id() const { return id; }
    const std::set<block_t, BlockHeightCmp> get_tails() const { return tails; }
    operator std::string () const;
    void set_neg_vote(bool _neg_vote) { neg_vote = _neg_vote; }
};


enum ProofType {
    VOTE = 0x00,
    BLAME = 0x01
};

/** Abstraction for proposal messages. */
struct Proposal: public Serializable {
    ReplicaID proposer;
    /** block being proposed */
    block_t blk;
    /** the cert for blk.parent; unused, for the future protocol */
    quorum_cert_bt cert_pblk;
    /** optional status messages (S) */
    status_cert_t status_cert;

    /** handle of the core object to allow polymorphism. The user should use
     * a pointer to the object of the class derived from HotStuffCore */
    HotStuffCore *hsc;

    Proposal(): blk(nullptr), cert_pblk(nullptr), status_cert(nullptr), hsc(nullptr) {}
    Proposal(ReplicaID proposer,
            const block_t &blk,
            quorum_cert_bt &&cert_pblk,
            status_cert_t &&status_cert,
            HotStuffCore *hsc):
        proposer(proposer),
        blk(blk), cert_pblk(std::move(cert_pblk)),
        status_cert(std::move(status_cert)),
        hsc(hsc) {}

    Proposal(const Proposal &other):
        proposer(other.proposer),
        blk(other.blk),
        cert_pblk(other.cert_pblk->clone()),
        status_cert(new std::vector(*other.status_cert)),
        hsc(other.hsc) {}

    void serialize(DataStream &s) const override {
        s << proposer
          << *blk << *cert_pblk;
        if (status_cert == nullptr)
            s << (uint8_t)0;
        else
        {
            s << (uint8_t)1;
            for (const auto &v: *status_cert) s << v;
        }
    }

    inline void unserialize(DataStream &s) override;

    promise_t verify(VeriPool &vpool) const;

    operator std::string () const {
        DataStream s;
        s << "<proposal "
          << "rid=" << std::to_string(proposer) << " "
          << "blk=" << get_hex10(blk->get_hash()) << " "
          << "status=" << (status_cert ? "yes" : "no") << ">";
        return std::move(s);
    }
};

/** Abstraction for vote messages. */
struct Vote: public Serializable {
    ReplicaID voter;
    /** block being voted */
    uint256_t blk_hash;
    /** proof of validity for the vote */
    part_cert_bt cert;
    
    /** handle of the core object to allow polymorphism */
    HotStuffCore *hsc;

    Vote(): cert(nullptr), hsc(nullptr) {}
    Vote(ReplicaID voter,
        const uint256_t &blk_hash,
        part_cert_bt &&cert,
        HotStuffCore *hsc):
        voter(voter),
        blk_hash(blk_hash),
        cert(std::move(cert)), hsc(hsc) {}

    Vote(const Vote &other):
        voter(other.voter),
        blk_hash(other.blk_hash),
        cert(other.cert->clone()),
        hsc(other.hsc) {}

    Vote(Vote &&other) = default;
    
    void serialize(DataStream &s) const override {
        s << voter << blk_hash << *cert;
    }

    void unserialize(DataStream &s) override {
        assert(hsc != nullptr);
        s >> voter >> blk_hash;
        cert = hsc->parse_part_cert(s);
    }

    static uint256_t proof_text_hash(const uint256_t &blk_hash) {
        DataStream p;
        p << (uint8_t)ProofType::VOTE << blk_hash;
        return p.get_hash();
    }

    bool verify() const {
        assert(hsc != nullptr);
        return cert->verify(hsc->get_config().get_pubkey(voter)) &&
                cert->get_blk_hash() == proof_text_hash(blk_hash);
    }

    promise_t verify(VeriPool &vpool) const {
        assert(hsc != nullptr);
        return cert->verify(hsc->get_config().get_pubkey(voter), vpool).then([this](bool result) {
            return result && cert->get_blk_hash() == proof_text_hash(blk_hash);
        });
    }

    operator std::string () const {
        DataStream s;
        s << "<vote "
          << "rid=" << std::to_string(voter) << " "
          << "blk=" << get_hex10(blk_hash) << ">";
        return std::move(s);
    }
};

struct Notify: public Serializable {
    uint256_t blk_hash;
    quorum_cert_bt qc;
    
    /** handle of the core object to allow polymorphism */
    HotStuffCore *hsc;

    Notify(): qc(nullptr), hsc(nullptr) {}
    Notify(const uint256_t blk_hash,
           quorum_cert_bt &&qc,
           HotStuffCore *hsc):
        blk_hash(blk_hash),
        qc(std::move(qc)), hsc(hsc) {}

    Notify(const Notify &other):
        blk_hash(other.blk_hash),
        qc(other.qc->clone()), hsc(other.hsc) {}

    Notify(Notify &&other) = default;
    
    void serialize(DataStream &s) const override {
        s << blk_hash << *qc;
    }

    void unserialize(DataStream &s) override {
        s >> blk_hash;
        qc = hsc->parse_quorum_cert(s);
    }

    bool verify() const {
        assert(hsc != nullptr);
        return qc->verify(hsc->get_config()) &&
            qc->get_blk_hash() == Vote::proof_text_hash(blk_hash);
    }

    promise_t verify(VeriPool &vpool) const {
        assert(hsc != nullptr);
        return qc->verify(hsc->get_config(), vpool).then([this](bool result) {
            return result && qc->get_blk_hash() == Vote::proof_text_hash(blk_hash);
        });
    }

    operator std::string () const {
        DataStream s;
        s << "<notify "
          << "blk=" << get_hex10(blk_hash) << ">";
        return std::move(s);
    }
};

struct Blame: public Serializable {
    ReplicaID blamer;
    uint32_t view;
    part_cert_bt cert;
    
    /** handle of the core object to allow polymorphism */
    HotStuffCore *hsc;

    Blame(): cert(nullptr), hsc(nullptr) {}
    Blame(ReplicaID blamer,
        uint32_t view,
        part_cert_bt &&cert,
        HotStuffCore *hsc):
        blamer(blamer),
        view(view),
        cert(std::move(cert)), hsc(hsc) {}

    Blame(const Blame &other):
        blamer(other.blamer),
        view(other.view),
        cert(other.cert->clone()),
        hsc(other.hsc) {}

    Blame(Blame &&other) = default;
    
    void serialize(DataStream &s) const override {
        s << blamer << view << *cert;
    }

    void unserialize(DataStream &s) override {
        assert(hsc != nullptr);
        s >> blamer >> view;
        cert = hsc->parse_part_cert(s);
    }

    static uint256_t proof_text_hash(uint32_t view) {
        DataStream p;
        p << (uint8_t)ProofType::BLAME << view;
        return p.get_hash();
    }

    bool verify() const {
        assert(hsc != nullptr);
        return cert->verify(hsc->get_config().get_pubkey(blamer)) &&
                cert->get_blk_hash() == proof_text_hash(view);
    }

    promise_t verify(VeriPool &vpool) const {
        assert(hsc != nullptr);
        return cert->verify(hsc->get_config().get_pubkey(blamer), vpool).then([this](bool result) {
            return result && cert->get_blk_hash() == proof_text_hash(view);
        });
    }

    operator std::string () const {
        DataStream s;
        s << "<blame "
          << "rid=" << std::to_string(blamer) << " "
          << "view=" << std::to_string(view) << ">";
        return std::move(s);
    }
};

struct BlameNotify: public Serializable {
    uint32_t view;
    quorum_cert_bt qc;
    
    /** handle of the core object to allow polymorphism */
    HotStuffCore *hsc;

    BlameNotify(): qc(nullptr), hsc(nullptr) {}
    BlameNotify(uint32_t view,
                quorum_cert_bt &&qc,
                HotStuffCore *hsc):
        view(view),
        qc(std::move(qc)), hsc(hsc) {}

    BlameNotify(const BlameNotify &other):
        view(other.view),
        qc(other.qc->clone()), hsc(other.hsc) {}

    BlameNotify(BlameNotify &&other) = default;
    
    void serialize(DataStream &s) const override {
        s << view << *qc;
    }

    void unserialize(DataStream &s) override {
        s >> view;
        qc = hsc->parse_quorum_cert(s);
    }

    bool verify() const {
        assert(hsc != nullptr);
        return qc->verify(hsc->get_config()) &&
            qc->get_blk_hash() == Blame::proof_text_hash(view);
    }

    promise_t verify(VeriPool &vpool) const {
        assert(hsc != nullptr);
        return qc->verify(hsc->get_config(), vpool).then([this](bool result) {
            return result && qc->get_blk_hash() == Blame::proof_text_hash(view);
        });
    }

    operator std::string () const {
        DataStream s;
        s << "<blame notify "
          << "view=" << std::to_string(view) << ">";
        return std::move(s);
    }
};

inline void Proposal::unserialize(DataStream &s) {
    assert(hsc != nullptr);
    uint8_t has_status;
    s >> proposer;
    Block _blk;
    _blk.unserialize(s, hsc);
    blk = hsc->storage->add_blk(std::move(_blk), hsc->get_config());
    cert_pblk = hsc->parse_quorum_cert(s);
    s >> has_status;
    if (has_status)
    {
        auto ns = hsc->get_config().nmajority;
        auto status_cert = status_cert_t(new std::vector<Notify>());
        status_cert->resize(ns);
        for (auto &v: *status_cert) s >> v;
    }
}

inline promise_t Proposal::verify(VeriPool &vpool) const {
    auto &config = hsc->get_config();
    assert(hsc != nullptr);
    std::vector<promise_t> pms{cert_pblk->verify(config, vpool)};
    for (auto &s: *status_cert)
        pms.push_back(s.verify(vpool));
    return promise::all(pms).then([this](const promise::values_t values) {
        bool result = cert_pblk->get_blk_hash() ==
            Vote::proof_text_hash(blk->get_parent_hashes()[0]);
        if (!result) return false;
        for (auto &v: values)
        {
            auto r = promise::any_cast<bool>(v);
            if (!(result &= r)) return false;
        }
        return true;
    });
}


struct Finality: public Serializable {
    ReplicaID rid;
    int8_t decision;
    uint32_t cmd_idx;
    uint32_t cmd_height;
    uint256_t cmd_hash;
    uint256_t blk_hash;
    
    public:
    Finality() = default;
    Finality(ReplicaID rid,
            int8_t decision,
            uint32_t cmd_idx,
            uint32_t cmd_height,
            uint256_t cmd_hash,
            uint256_t blk_hash):
        rid(rid), decision(decision),
        cmd_idx(cmd_idx), cmd_height(cmd_height),
        cmd_hash(cmd_hash), blk_hash(blk_hash) {}

    void serialize(DataStream &s) const override {
        s << rid << decision
          << cmd_idx << cmd_height
          << cmd_hash;
        if (decision == 1) s << blk_hash;
    }

    void unserialize(DataStream &s) override {
        s >> rid >> decision
          >> cmd_idx >> cmd_height
          >> cmd_hash;
        if (decision == 1) s >> blk_hash;
    }

    operator std::string () const {
        DataStream s;
        s << "<fin "
          << "decision=" << std::to_string(decision) << " "
          << "cmd_idx=" << std::to_string(cmd_idx) << " "
          << "cmd_height=" << std::to_string(cmd_height) << " "
          << "cmd=" << get_hex10(cmd_hash) << " "
          << "blk=" << get_hex10(blk_hash) << ">";
        return std::move(s);
    }
};

}

#endif
