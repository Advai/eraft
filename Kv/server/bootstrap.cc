#include <Kv/bootstrap.h>
#include <Kv/utils.h>
#include <eraftio/raft_serverpb.pb.h>
#include <eraftio/metapb.pb.h>

#include <Logger/Logger.h>

namespace kvserver
{

BootHelper* BootHelper::instance_= nullptr;
uint64_t BootHelper::gCounter_ = 0;

bool BootHelper::IsRangeEmpty(rocksdb::DB* db, std::string startKey, std::string endKey)
{
    bool hasData;
    hasData = false;
    auto it = db->NewIterator(rocksdb::ReadOptions());
    it->Seek(startKey);
    if(it->Valid())
    {
        if(it->key().ToString().compare(endKey) < 0) 
        {
            hasData = true;
        }
    }
    return !hasData;
}

uint64_t BootHelper::MockSchAllocID()
{
    gCounter_++;
    return gCounter_;
}

BootHelper* BootHelper::GetInstance()
{
    if(instance_ == nullptr)
    {
        instance_ = new BootHelper();
        return instance_;
    }
}

bool BootHelper::DoBootstrapStore(std::shared_ptr<Engines> engines, uint64_t clusterID, uint64_t storeID, std::string storeAddr)
{
    auto ident = new raft_serverpb::StoreIdent();
    if(!IsRangeEmpty(engines->kvDB_, "", 
        std::string(Assistant::GetInstance()->MaxKey.begin(), Assistant::GetInstance()->MaxKey.end())))
    {
        Logger::GetInstance()->DEBUG_NEW("err: kv db is not empty", __FILE__, __LINE__, "BootHelper::DoBootstrapStore");
        return false;
    }
    if(!IsRangeEmpty(engines->raftDB_, "", 
        std::string(Assistant::GetInstance()->MaxKey.begin(), Assistant::GetInstance()->MaxKey.end())))
    {
        Logger::GetInstance()->DEBUG_NEW("err: raft db is not empty", __FILE__, __LINE__, "BootHelper::DoBootstrapStore");
        return false;
    }
    ident->set_cluster_id(clusterID);
    ident->set_store_id(storeID);
    Assistant::GetInstance()->PutMeta(engines->kvDB_, std::string(Assistant::GetInstance()->StoreIdentKey.begin(), Assistant::GetInstance()->StoreIdentKey.end()), *ident);
    Logger::GetInstance()->DEBUG_NEW("do bootstrap store successful", __FILE__, __LINE__, "BootHelper::DoBootstrapStore");
    return true;
}

std::pair<std::shared_ptr<metapb::Region>, bool> BootHelper::PrepareBootstrap(std::shared_ptr<Engines> engines, std::string storeAddr, std::map<std::string, int> peerAddrMaps)
{
    std::shared_ptr<metapb::Region> region = std::make_shared<metapb::Region>();

    // add all peers to region
    for(auto item: peerAddrMaps)
    {
        if(item.first == storeAddr)
        {
            region->mutable_region_epoch()->set_version(kInitEpochVer);
            region->mutable_region_epoch()->set_conf_ver(kInitEpochConfVer);
            auto addPeer = region->add_peers(); // add peer to region
            addPeer->set_id(item.second);
            addPeer->set_store_id(item.second);
            addPeer->set_addr(item.first);
            region->set_id(1);
            region->set_start_key("");
            region->set_end_key("");
            Logger::GetInstance()->DEBUG_NEW("bootstrap node with regionID: " + std::to_string(item.second) + 
            "  storeID: " + std::to_string(item.second) + " peerID: " + std::to_string(item.second), __FILE__, __LINE__, "BootHelper::PrepareBootstrap");
            continue;
        }
        auto addNewPeer = region->add_peers(); // add peer to region
        addNewPeer->set_id(item.second);
        addNewPeer->set_store_id(item.second);
        addNewPeer->set_addr(item.first);
    }
    assert(PrepareBoostrapCluster(engines, region));
    return std::make_pair(region, true);
}

bool BootHelper::PrepareBoostrapCluster(std::shared_ptr<Engines> engines, std::shared_ptr<metapb::Region> region)
{
    raft_serverpb::RegionLocalState* state = new raft_serverpb::RegionLocalState();
    state->set_allocated_region(region.get());
    rocksdb::WriteBatch kvWB;
    std::string prepareBootstrapKey(Assistant::GetInstance()->PrepareBootstrapKey.begin(), Assistant::GetInstance()->PrepareBootstrapKey.end());
    Assistant::GetInstance()->SetMeta(&kvWB, prepareBootstrapKey, *state);
    Assistant::GetInstance()->SetMeta(&kvWB, Assistant::GetInstance()->RegionStateKey(region->id()), *state);
    WriteInitialApplyState(&kvWB, region->id());
    engines->kvDB_->Write(rocksdb::WriteOptions(), &kvWB);
    rocksdb::WriteBatch raftWB;
    WriteInitialRaftState(&raftWB, region->id());
    engines->raftDB_->Write(rocksdb::WriteOptions(), &raftWB);
    Logger::GetInstance()->DEBUG_NEW("do prepare boostrap cluster successful", __FILE__, __LINE__, "BootHelper::PrepareBoostrapCluster");
    return true;
}

// write initial apply state to rocksdb batch kvWB
void BootHelper::WriteInitialApplyState(rocksdb::WriteBatch* kvWB, uint64_t regionID)
{
    raft_serverpb::RaftApplyState* applyState = new raft_serverpb::RaftApplyState();
    raft_serverpb::RaftTruncatedState* truncatedState = new raft_serverpb::RaftTruncatedState();
    applyState->set_applied_index(Assistant::GetInstance()->kRaftInitLogIndex);
    truncatedState->set_index(Assistant::GetInstance()->kRaftInitLogIndex);
    truncatedState->set_term(Assistant::GetInstance()->kRaftInitLogTerm);
    applyState->set_allocated_truncated_state(truncatedState);
    Assistant::GetInstance()->SetMeta(kvWB, Assistant::GetInstance()->ApplyStateKey(regionID), *applyState);
}

// write initial raft state to raft batch, logindex = 5, logterm = 5
void BootHelper::WriteInitialRaftState(rocksdb::WriteBatch* raftWB, uint64_t regionID)
{
    raft_serverpb::RaftLocalState* raftState = new raft_serverpb::RaftLocalState();
    eraftpb::HardState hardState;
    hardState.set_term(Assistant::GetInstance()->kRaftInitLogTerm);
    hardState.set_commit(Assistant::GetInstance()->kRaftInitLogIndex);
    raftState->set_last_index(Assistant::GetInstance()->kRaftInitLogIndex);
    Assistant::GetInstance()->SetMeta(raftWB, Assistant::GetInstance()->RaftStateKey(regionID), *raftState);
}

bool BootHelper::ClearPrepareBoostrap(std::shared_ptr<Engines> engines, uint64_t regionID)
{
    engines->raftDB_->Delete(rocksdb::WriteOptions(), Assistant::GetInstance()->RaftStateKey(regionID));
    std::shared_ptr<rocksdb::WriteBatch> wb = std::make_shared<rocksdb::WriteBatch>();
    wb->Delete(Assistant::GetInstance()->VecToString(Assistant::GetInstance()->PrepareBootstrapKey));
    wb->Delete(Assistant::GetInstance()->RegionStateKey(regionID));
    wb->Delete(Assistant::GetInstance()->ApplyStateKey(regionID));
    engines->kvDB_->Write(rocksdb::WriteOptions(),& *wb);
    return true;
}

bool BootHelper::ClearPrepareBoostrapState(std::shared_ptr<Engines> engines)
{
    engines->kvDB_->Delete(rocksdb::WriteOptions(), Assistant::GetInstance()->VecToString(Assistant::GetInstance()->PrepareBootstrapKey));
    return true;
}

} // namespace kvserver
