#ifndef ERAFT_KV_PEER_H_
#define ERAFT_KV_PEER_H_

#include <RaftCore/RawNode.h>
#include <eraftio/metapb.pb.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <map>
#include <memory>

#include <Kv/callback.h>
#include <Kv/engines.h>
#include <Kv/transport.h>
#include <Kv/config.h>

namespace kvserver
{

class PeerStorage;

struct Proposal
{
    uint64_t index_;

    uint64_t term_;

    Callback* db_;
};


class Peer
{

friend class Router;

public:

    Peer();
    Peer(uint64_t storeID, std::shared_ptr<Config> cfg, std::shared_ptr<Engines> engines, std::shared_ptr<metapb::Region> region);
    ~Peer();

    void InsertPeerCache(std::shared_ptr<metapb::Peer> peer);

    void RemovePeerCache(uint64_t peerID);

    std::shared_ptr<metapb::Peer> GetPeerFromCache(uint64_t peerID);

    uint64_t NextProposalIndex();

    bool MaybeDestory();

    bool Destory(std::shared_ptr<Engines> engine, bool keepData);

    bool IsInitialized();

    uint64_t storeID();

    std::shared_ptr<metapb::Region> Region();

    void SetRegion(std::shared_ptr<metapb::Region> region);

    uint64_t PeerId();

    uint64_t LeaderId();

    bool IsLeader();

    void Send(Transport trans, std::vector<eraftpb::Message> msgs);

    std::vector<std::shared_ptr<metapb::Peer> > CollectPendingPeers();

    void ClearPeersStartPendingTime();

    bool AnyNewPeerCatchUp(uint64_t peerId);

    bool MaydeCampaign(bool parentIsLeader);

    uint64_t Term();

    void HeartbeatScheduler(); // heart beat

    bool SendRaftMessage(eraftpb::Message msg, Transport trans);

    // ticker

    // instance of the raft moudle
    eraft::RawNode* raftGroup;

    // peer storage
    PeerStorage* peerStorage;

    metapb::Peer* meta;

private:

    uint64_t regionId_;

    std::string tag_;

    std::vector<Proposal> proposals_;

    uint64_t lastCompactedIdx_;

    std::map<uint64_t, metapb::Peer* > peerCache_;
    
    bool stopped_;

    uint64_t sizeDiffHint_;

    uint64_t approximateSize_;

};


} // namespace kvserver


#endif