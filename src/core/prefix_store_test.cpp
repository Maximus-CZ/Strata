#include "strata/core/prefix_store.hpp"
#include "strata/kernels/kv_q4.hpp"
#include <cuda_runtime.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>
#include <limits>
#include <vector>

using namespace strata::core;
using namespace strata::kernels;

namespace {
int checks = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
void cuda_check(cudaError_t e) {
    if (e != cudaSuccess) { std::fprintf(stderr, "CUDA: %s\n", cudaGetErrorString(e)); std::exit(1); }
}
struct Fixture {
    ModelGeometry g;
    QsaState state;
    std::vector<void*> device, host;
    std::array<void*,5> sources{};
    std::array<size_t,5> sizes{};

    template<class T> void alloc(T*& p, size_t n, bool pinned = false) {
        if (!n) return;
        void* raw = nullptr;
        if (pinned) {
            cuda_check(cudaHostAlloc(&raw, n, cudaHostAllocMapped));
            host.push_back(raw);
            void* mapped = nullptr;
            cuda_check(cudaHostGetDevicePointer(&mapped, raw, 0));
            p = static_cast<T*>(mapped);
        } else {
            cuda_check(cudaMalloc(&raw, n)); device.push_back(raw); p = static_cast<T*>(raw);
        }
    }
    Fixture(int fmt, int mode) {
        auto& st = state;
        st.kv_mode = mode; st.kv_int8 = fmt==kKvInt8; st.kv_q4 = fmt==kKvQ4; st.kv_hybrid = fmt==3;
        st.n_pages = 24; st.n_slots = mode ? 4 : st.n_pages;
        st.max_cells = st.n_pages * 4; st.idx_pooled_rows = mode==2 ? 2 : st.max_cells/4+2;
        const size_t per = fmt==kKvQ4 ? kv_q4_bytes_per_head((int)g.head_dim) : g.head_dim*((st.kv_int8 || st.kv_hybrid) ? 1:2);
        const size_t rows = st.max_cells*g.n_head_kv, slot_rows = st.n_slots*4*g.n_head_kv;
        sizes = {rows*per, rows*per, fmt==kKvInt8 ? rows*(g.head_dim/64)*2:0,
                 fmt==kKvInt8 ? rows*(g.head_dim/64)*2:0, (size_t)st.idx_pooled_rows*g.idx_key_dim*4};
        if (fmt==3) {
            sizes = {rows*per, rows*kv_q4_bytes_per_head((int)g.head_dim), rows*(g.head_dim/64)*2, 0,
                     (size_t)st.idx_pooled_rows*g.idx_key_dim*4};
            alloc(st.k_q,sizes[0]); alloc(st.v_q4,sizes[1]); alloc(st.k_scale,sizes[2]);
            sources[0]=st.k_q; sources[1]=st.v_q4; sources[2]=st.k_scale;
        } else if (fmt==kKvQ4) {
            alloc(st.k_q4,slot_rows*per); alloc(st.v_q4,slot_rows*per);
            if (mode) {alloc(st.host.k_q4,sizes[0],true); alloc(st.host.v_q4,sizes[1],true);}
            sources[0]=mode?st.host.k_q4:st.k_q4; sources[1]=mode?st.host.v_q4:st.v_q4;
        } else if (fmt==kKvInt8) {
            alloc(st.k_q,slot_rows*per); alloc(st.v_q,slot_rows*per);
            alloc(st.k_scale,slot_rows*(g.head_dim/64)*2); alloc(st.v_scale,slot_rows*(g.head_dim/64)*2);
            if (mode) {
                alloc(st.host.k_q,sizes[0],true); alloc(st.host.v_q,sizes[1],true);
                alloc(st.host.k_scale,sizes[2],true); alloc(st.host.v_scale,sizes[3],true);
            }
            sources[0]=mode?st.host.k_q:st.k_q; sources[1]=mode?st.host.v_q:st.v_q;
            sources[2]=mode?st.host.k_scale:st.k_scale; sources[3]=mode?st.host.v_scale:st.v_scale;
        } else {
            alloc(st.k_pool,slot_rows*per); alloc(st.v_pool,slot_rows*per);
            if (mode) {alloc(st.host.k_pool,sizes[0],true); alloc(st.host.v_pool,sizes[1],true);}
            sources[0]=mode?st.host.k_pool:st.k_pool; sources[1]=mode?st.host.v_pool:st.v_pool;
        }
        alloc(st.idx_pooled,sizes[4]); sources[4]=st.idx_pooled;
        alloc(st.page_table,st.n_pages*4);
        if (mode==1) {
            auto& m=st.map;
            m.page_table=st.page_table; m.n_blocks=st.n_pages; m.n_slots=st.n_slots;
            alloc(m.slot_block,st.n_slots*4); alloc(m.slot_stamp,st.n_slots*4); alloc(m.slot_ref,st.n_slots*4);
            alloc(m.miss_block,st.n_slots*4); alloc(m.miss_slot,st.n_slots*4); alloc(m.ctl,kKvCtlInts*4);
        } else if (mode==2) {
            kv_ring_table(st.page_table,st.n_pages,st.n_slots,nullptr);
        }
    }
    void fill(uint8_t salt) {
        for (size_t i=0;i<sources.size();++i) {
            std::vector<uint8_t> data(sizes[i]);
            for (size_t j=0;j<data.size();++j) data[j]=(uint8_t)(salt+i*31+j*7+j/257);
            if (!data.empty()) cuda_check(cudaMemcpy(sources[i],data.data(),data.size(),cudaMemcpyDefault));
        }
    }
    void fill_after(uint8_t salt, int64_t first_dirty) {
        for (size_t i=0;i<sources.size();++i) {
            const size_t offset = i == 4 ? size_t(first_dirty/4)*g.idx_key_dim*4
                                         : (sizes[i]/size_t(state.max_cells))*size_t((first_dirty/4)*4);
            if (sizes[i] > offset)
                cuda_check(cudaMemset(static_cast<uint8_t*>(sources[i])+offset, salt, sizes[i]-offset));
        }
    }
    ~Fixture() { for (void* p:device) cudaFree(p); for (void* p:host) cudaFreeHost(p); }
};
bool equal(const ConversationKv& a,const ConversationKv& b) {
    return a.k==b.k && a.v==b.v && a.k_scale==b.k_scale && a.v_scale==b.v_scale && a.pooled==b.pooled;
}
bool same_state(const SavedConversation& a,const SavedConversation& b) {
    if (a.kv.size()!=b.kv.size() || a.live.ids!=b.live.ids) return false;
    for (size_t i=0;i<a.kv.size();++i) if (!equal(a.kv[i],b.kv[i])) return false;
    return a.live.gdn==b.live.gdn && a.live.ple==b.live.ple && a.live.tails==b.live.tails &&
           a.live.dead==b.live.dead && a.live.block_pos==b.live.block_pos;
}

// docs/DETAILS.md: a prefix saved from a session that holds more than it comes back byte for byte by every
// path (streamed from the file, read into RAM, from RAM), the index survives a restart, and files of another
// identity, over the disk budget or cut short are dropped.
void prefix_session(int fmt, int mode, const std::string& dir) {
    namespace fs = std::filesystem;
    fs::remove_all(dir);
    Fixture main(fmt,mode), draft(fmt==3?kKvInt8:fmt,2);
    auto& g=main.g;
    g.n_layers=4; g.n_expert=256;
    g.ssm_state_size=2; g.ssm_v_heads=2; g.ssm_conv_channels=8;
    SessionState ss;
    ss.max_cells=96; ss.qsa_states=&main.state;
    ss.layer_hi=g.n_layers; ss.gdn_alloc=g.n_gdn_layers(); ss.qsa_alloc=g.n_qsa_layers();
    std::string err,log,source;
    ConversationStateSizes sizes;
    check(conversation_state_sizes(g,sizes,err),"geometry sizes");
    main.alloc(ss.gdn_state,sizes.gdn); main.alloc(ss.ple_hist,sizes.ple);
    main.alloc(main.state.idx_tail,sizes.tail); main.alloc(main.state.idx_dead,sizes.dead);
    main.alloc(main.state.idx_block_pos,sizes.block_pos);
    std::vector<int32_t> ids(65);
    for (size_t i=0;i<ids.size();++i) ids[i]=(int32_t)i+1;
    auto fill=[&](uint8_t salt) {
        main.fill(salt); draft.fill(salt);
        for (const auto& [p,n] : std::vector<std::pair<void*,size_t>>{
                 {ss.gdn_state,sizes.gdn},{ss.ple_hist,sizes.ple},{main.state.idx_tail,sizes.tail},
                 {main.state.idx_dead,sizes.dead},{main.state.idx_block_pos,sizes.block_pos}})
            cuda_check(cudaMemset(p,salt,n));
        cuda_check(cudaDeviceSynchronize());
    };
    const std::vector<ConversationImageKey> none;
    const std::vector<ConversationCheckpoint> no_checks;
    // the state at a root of `L` tokens, as a whole-session image: what a restore must reproduce
    auto at_root=[&](uint8_t salt,int64_t L,ConversationCheckpoint& root,SavedConversation& ref) {
        fill(salt);
        root={}; root.ids.assign(ids.begin(),ids.begin()+L);
        check(conversation_checkpoint_save(root,ss,g,err),"save root checkpoint");
        check(conversation_checkpoint_restore(root,ss,g,err),"root spare row");
        const ConversationView view{root.ids,none,no_checks,true};
        check(conversation_snapshot_save(ref,view,ss,g,draft.state,err),"reference image");
        fill(salt);   // the session goes on past the root
    };
    auto restored=[&](const SavedConversation& ref) {
        SavedConversation now;
        const ConversationView view{ref.live.ids,none,no_checks,true};
        cuda_check(cudaDeviceSynchronize());
        check(conversation_snapshot_save(now,view,ss,g,draft.state,err),"read back");
        return same_state(now,ref);
    };
    PrefixStore::Options o;
    o.dir=dir; o.identity="engine A"; o.disk_budget=size_t(1)<<30; o.bounce=4096;   // many chunks per payload
    ConversationCheckpoint root;
    SavedConversation ref;
    at_root(13,40,root,ref);
    {
        PrefixStore store;
        check(store.open(o,log,err) && store.size()==0,"open empty directory");
        check(store.capture(root,true,ss,g,draft.state,log,err) && store.size()==1,"capture straight to disk");
        check(store.contains(root.ids,{},true) && !store.contains(root.ids,{},false),"cvec is part of the identity");
        check(store.best(ids,{},true).tokens==40 && store.best(ids,{},false).tokens==0,"longest prefix match");
        check(store.best(root.ids,{},true).tokens==0,"never the whole prompt");
        fill(177);
        check(store.restore(store.best(ids,{},true).key,ss,g,draft.state,source,err)==ConversationRestore::restored &&
              source=="disk","no RAM budget: streamed from the file");
        check(restored(ref),"streamed restore is exact");
    }
    o.ram_budget=size_t(1)<<30;
    {
        PrefixStore store;
        check(store.open(o,log,err) && store.size()==1,"the index survives a restart");
        const auto key=store.best(ids,{},true).key;
        fill(91);
        check(store.restore(key,ss,g,draft.state,source,err)==ConversationRestore::restored && source=="disk>ram",
              "read into the RAM tier");
        check(restored(ref) && store.ram_bytes()>0,"exact through the RAM tier");
        fill(92);
        check(store.restore(key,ss,g,draft.state,source,err)==ConversationRestore::restored && source=="ram",
              "then from RAM");
        check(restored(ref),"exact from RAM");
        // a disk budget for one: the second prefix pushes out the least recently used
        ConversationCheckpoint root2;
        SavedConversation ref2;
        at_root(29,20,root2,ref2);
        PrefixStore::Options one=o;
        one.disk_budget=store.disk_bytes()+store.disk_bytes()/4;
        PrefixStore small;
        check(small.open(one,log,err) && small.size()==1,"reopen with a small budget");
        check(small.capture(root2,true,ss,g,draft.state,log,err) && small.size()==1 &&
              small.best(ids,{},true).tokens==20,"the older prefix leaves for the newer");
        fill(93);
        check(small.restore(small.best(ids,{},true).key,ss,g,draft.state,source,err)==ConversationRestore::restored &&
              restored(ref2),"the newer prefix restores");
    }
    {
        PrefixStore::Options other=o;
        other.identity="engine B";
        PrefixStore store;
        check(store.open(other,log,err) && store.size()==0,"another engine or model deletes the files");
        check(std::distance(fs::directory_iterator(dir),fs::directory_iterator{})==0,"and nothing is left");
        check(store.capture(root,true,ss,g,draft.state,log,err),"capture again");
    }
    {
        PrefixStore::Options other=o;
        other.identity="engine B";
        for (const auto& d : fs::directory_iterator(dir)) fs::resize_file(d.path(),fs::file_size(d.path())-1);
        PrefixStore store;
        check(store.open(other,log,err) && store.size()==0,"a file cut short is dropped");
    }
    {   // local patch (delta end points): an end point is stored as the bytes past an older snapshot of its tokens
        at_root(13,40,root,ref);
        PrefixStore::Options d=o;
        d.ram_budget=0;
        PrefixStore store;
        check(store.open(d,log,err) && store.size()==0 && store.capture(root,true,ss,g,draft.state,log,err),
              "delta: the root");
        const size_t whole=store.disk_bytes();
        check(store.restore(store.best(ids,{},true).key,ss,g,draft.state,source,err)==ConversationRestore::restored,
              "delta: the session holds the root's bytes");
        // the conversation goes on: cells from 40 on are new, then an end point of 60 tokens
        auto end_point=[&](uint8_t salt,int64_t from,int64_t L,ConversationCheckpoint& c,SavedConversation& r) {
            main.fill_after(salt,from); draft.fill_after(salt,from);
            cuda_check(cudaDeviceSynchronize());
            c={}; c.ids.assign(ids.begin(),ids.begin()+L);
            check(conversation_checkpoint_save(c,ss,g,err),"delta: checkpoint");
            check(conversation_checkpoint_restore(c,ss,g,err),"delta: spare row");
            const ConversationView view{c.ids,none,no_checks,true};
            check(conversation_snapshot_save(r,view,ss,g,draft.state,err),"delta: reference");
        };
        ConversationCheckpoint e1,e2,e3;
        SavedConversation r1,r2,r3;
        end_point(55,40,60,e1,r1);
        check(store.capture(e1,true,ss,g,draft.state,log,err) && log.find("a delta on 40")!=std::string::npos &&
              store.disk_bytes()-whole<whole,"delta: the end point is a delta on the root");
        end_point(77,60,64,e2,r2);
        check(store.capture(e2,true,ss,g,draft.state,log,err) && log.find("a delta on 60")!=std::string::npos &&
              log.find("chain of 3")!=std::string::npos,"delta: the next end point on the previous one");
        check(store.drop_superseded(e2.ids,{},40,true)==0 && store.size()==3,"delta: a base is not dropped");
        fill(201);
        check(store.restore(store.best(ids,{},true).key,ss,g,draft.state,source,err)==ConversationRestore::restored &&
              source=="disk, 3 files" && restored(r2),"delta: a chain of three restores exactly");
        fill(202);
        std::vector<int32_t> p61(ids.begin(),ids.begin()+61);
        check(store.restore(store.best(p61,{},true).key,ss,g,draft.state,source,err)==ConversationRestore::restored &&
              restored(r1),"delta: the middle one too");
        // other bytes for the same tokens (computed elsewhere): saved whole, not on them
        fill(99);
        e3={}; e3.ids.assign(ids.begin(),ids.begin()+50);
        check(conversation_checkpoint_save(e3,ss,g,err) && conversation_checkpoint_restore(e3,ss,g,err),"delta: e3");
        {
            const ConversationView view{e3.ids,none,no_checks,true};
            check(conversation_snapshot_save(r3,view,ss,g,draft.state,err),"delta: e3 reference");
        }
        check(store.capture(e3,true,ss,g,draft.state,log,err) && log.find("delta")==std::string::npos,
              "delta: different bytes are saved whole");
        PrefixStore again;
        check(again.open(d,log,err) && again.size()==4,"delta: the chain survives a restart");
        fill(203);
        check(again.restore(again.best(ids,{},true).key,ss,g,draft.state,source,err)==ConversationRestore::restored &&
              restored(r2),"delta: and restores exactly after it");
        fill(204);
        std::vector<int32_t> p51(ids.begin(),ids.begin()+51);
        check(again.restore(again.best(p51,{},true).key,ss,g,draft.state,source,err)==ConversationRestore::restored &&
              source=="disk" && restored(r3),"delta: the whole one restores exactly");
        check(again.drop_superseded(std::vector<int32_t>(ids.begin(),ids.begin()+65),{},40,true)==3 &&
              again.size()==1,"delta: superseded end points go once nothing is stored on them");
        // siblings: questions on one root share a few tokens past it, computed in other sessions (other bytes).
        // Three of them, then a fourth: it must still be a delta on the root, not saved whole (2026-10-16)
        auto sibling=[&](uint8_t salt,int k,ConversationCheckpoint& c,SavedConversation& r) {
            fill(240);
            check(again.restore(again.best(ids,{},true).key,ss,g,draft.state,source,err)==ConversationRestore::restored,
                  "siblings: from the root");
            main.fill_after(salt,40); draft.fill_after(salt,40);
            cuda_check(cudaDeviceSynchronize());
            c={}; c.ids.assign(ids.begin(),ids.begin()+49);
            for (int j=0;j<11;++j) c.ids.push_back(1000*k+j);
            check(conversation_checkpoint_save(c,ss,g,err) && conversation_checkpoint_restore(c,ss,g,err),"siblings: point");
            const ConversationView view{c.ids,none,no_checks,true};
            check(conversation_snapshot_save(r,view,ss,g,draft.state,err),"siblings: reference");
        };
        ConversationCheckpoint s1,s2,s3,s4;
        SavedConversation q1,q2,q3,q4;
        sibling(71,1,s1,q1); check(again.capture(s1,true,ss,g,draft.state,log,err),"siblings: 1");
        sibling(72,2,s2,q2); check(again.capture(s2,true,ss,g,draft.state,log,err),"siblings: 2");
        sibling(73,3,s3,q3); check(again.capture(s3,true,ss,g,draft.state,log,err),"siblings: 3");
        sibling(74,4,s4,q4);
        check(again.capture(s4,true,ss,g,draft.state,log,err) && log.find("a delta on 40")!=std::string::npos,
              "siblings: the fourth is a delta on the root, not on a sibling or whole");
        fill(241);
        std::vector<int32_t> next=s4.ids; next.push_back(9);
        // local patch (2026-10-18): the store evicts the least recently used snapshot of the LARGEST family first
        {
            PrefixStore::Options f=d; f.family_tokens=8;
            const std::string fdir=dir+"_family"; fs::remove_all(fdir); f.dir=fdir;
            PrefixStore fam;
            check(fam.open(f,log,err),"family: open");
            auto point=[&](int32_t first,int64_t L) {   // a snapshot whose first 8 tokens are first..first+7
                ConversationCheckpoint c; c.ids.resize((size_t)L);
                for (int64_t i=0;i<L;++i) c.ids[(size_t)i]=first+(int32_t)i;
                check(conversation_checkpoint_save(c,ss,g,err) && fam.capture(c,true,ss,g,draft.state,log,err),"family: save");
                return c;
            };
            fill(250);
            auto old_b=point(5000,30);                  // family B: one snapshot, the oldest of all
            point(100,30); point(100,34); point(100,38); // family A: three, newer
            const size_t one=fam.disk_bytes()/4;
            f.disk_budget=fam.disk_bytes()+one/2;        // room for one more only after an eviction
            PrefixStore tight; check(tight.open(f,log,err) && tight.size()==4,"family: reopen with a tight budget");
            fill(251);
            ConversationCheckpoint c; c.ids.resize(42); for (int i=0;i<42;++i) c.ids[(size_t)i]=100+i;
            check(conversation_checkpoint_save(c,ss,g,err) && tight.capture(c,true,ss,g,draft.state,log,err),"family: A grows");
            check(tight.contains(old_b.ids,{},true),"family: the oldest snapshot of the small family stays");
            check(tight.size()<=4,"family: one of the large family's went instead");
            fs::remove_all(fdir);
        }
        check(again.best(next,{},true).tokens==(int64_t)s4.ids.size() &&
              again.restore(again.best(next,{},true).key,ss,g,draft.state,source,err)==ConversationRestore::restored &&
              restored(q4),"siblings: and it restores exactly");
    }
    {   // local patch (2026-10-21): an end point written from a PARKED image, after the session moved on to another
        // conversation (the RAM cache letting it go), is the same snapshot as one captured from the session at park time
        const std::string adir=dir+"_gpu", bdir=dir+"_image";
        fs::remove_all(adir); fs::remove_all(bdir);
        PrefixStore::Options oa=o, ob=o;
        oa.dir=adir; ob.dir=bdir; oa.ram_budget=ob.ram_budget=0;
        PrefixStore a, b;
        check(a.open(oa,log,err) && b.open(ob,log,err),"image: open");
        ConversationCheckpoint r0; SavedConversation rr0;
        at_root(13,40,r0,rr0);
        check(a.capture(r0,true,ss,g,draft.state,log,err) && b.capture(r0,true,ss,g,draft.state,log,err),"image: roots");
        check(b.restore(b.best(ids,{},true).key,ss,g,draft.state,source,err)==ConversationRestore::restored,
              "image: the session at the root");
        main.fill_after(66,40); draft.fill_after(66,40);
        cuda_check(cudaDeviceSynchronize());
        ConversationCheckpoint c; c.ids.assign(ids.begin(),ids.begin()+60);
        check(conversation_checkpoint_save(c,ss,g,err) && conversation_checkpoint_restore(c,ss,g,err),"image: end point");
        main.fill_after(67,60); draft.fill_after(67,60);   // the conversation goes on to 64 tokens
        cuda_check(cudaDeviceSynchronize());
        const std::vector<int32_t> live64(ids.begin(),ids.begin()+64);
        SavedConversation parked;
        {
            const std::vector<ConversationCheckpoint> cks{c};
            const ConversationView v{live64,none,cks,true};
            check(conversation_snapshot_save(parked,v,ss,g,draft.state,err) && parked.checkpoints.size()==1,"image: parked");
        }
        check(a.capture(c,true,ss,g,draft.state,log,err),"image: the end point from the session (park time)");
        fill(31);                                          // the session moves on to another conversation
        check(b.capture_image(parked.checkpoints[0],parked,ss,g,draft.state,log,err) &&
              log.find("a delta on 40")!=std::string::npos,"image: written from RAM, a delta on the root");
        std::vector<int32_t> p61(ids.begin(),ids.begin()+61);
        fill(32);
        check(a.restore(a.best(p61,{},true).key,ss,g,draft.state,source,err)==ConversationRestore::restored,"image: a");
        SavedConversation from_gpu;
        {
            const ConversationView v{c.ids,none,no_checks,true};
            cuda_check(cudaDeviceSynchronize());
            check(conversation_snapshot_save(from_gpu,v,ss,g,draft.state,err),"image: a's state");
        }
        fill(33);
        check(b.restore(b.best(p61,{},true).key,ss,g,draft.state,source,err)==ConversationRestore::restored &&
              restored(from_gpu),"image: restores to exactly the same state as the session's own capture");
        fs::remove_all(adir); fs::remove_all(bdir);
    }
    fs::remove_all(dir);
}
}

int main() {
    int devices=0;
    if (cudaGetDeviceCount(&devices)!=cudaSuccess || !devices) return 77;
    const std::string dir=(std::filesystem::temp_directory_path()/"strata_prefix_store_test").string();
    for (int fmt : {kKvF16,kKvInt8,kKvQ4}) for (int mode : {0,1}) prefix_session(fmt,mode,dir);
    prefix_session(3,0,dir);
    std::printf("prefix_store_test: %d checks passed\n",checks);
}
