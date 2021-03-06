/**
* Non-metric Space Library
*
* Authors: Bilegsaikhan Naidan (https://github.com/bileg), Leonid Boytsov (http://boytsov.info).
* With contributions from Lawrence Cayton (http://lcayton.com/) and others.
*
* For the complete list of contributors and further details see:
* https://github.com/searchivarius/NonMetricSpaceLib
*
* Copyright (c) 2014
*
* This code is released under the
* Apache License Version 2.0 http://www.apache.org/licenses/.
*
*/

/*
*
* A Hierarchical Navigable Small World (HNSW) approach.
*
* The main publication is (available on arxiv: http://arxiv.org/abs/1603.09320):
* "Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs" by Yu. A. Malkov, D. A. Yashunin
* This code was contributed by Yu. A. Malkov. It also was used in tests from the paper.
*
*
*/

#include <cmath>
#include <memory>
#include <iostream>
#include <mmintrin.h>

#include "simd.h"
#include "space.h"
#include "knnquery.h"
#include "rangequery.h"
#include "ported_boost_progress.h"
#include "method/hnsw.h"
#include "space/space_lp.h"

#include <vector>
#include <set>
#include <map>
#include <sstream>
#include <typeinfo>

#include "sort_arr_bi.h"
#define MERGE_BUFFER_ALGO_SWITCH_THRESHOLD 100

#ifdef _OPENMP
#include <omp.h>
#endif

#define USE_BITSET_FOR_INDEXING 1
#define EXTEND_USE_EXTENDED_NEIGHB_AT_CONSTR (0) // 0 is faster build, 1 is faster search on clustered data

#if defined(__GNUC__)
#define PORTABLE_ALIGN16 __attribute__((aligned(16)))
#else
#define PORTABLE_ALIGN16 __declspec(align(16))
#endif

namespace similarity {

// This is the counter to keep the size of neighborhood information (for one node)
// TODO Can this one overflow? I really doubt
    typedef uint32_t SIZEMASS_TYPE;

    using namespace std;
    /*Functions from hnsw_distfunc_opt.cc:*/
    float L2SqrSIMDExt(const float* pVect1, const float* pVect2, size_t &qty, float *TmpRes);
    float L2SqrSIMD16Ext(const float* pVect1, const float* pVect2, size_t &qty, float *TmpRes);
    float NormScalarProductSIMD(const float* pVect1, const float* pVect2, size_t &qty, float *TmpRes);

    template <typename dist_t>    Hnsw<dist_t>::Hnsw(bool PrintProgress, const Space<dist_t>& space,   const ObjectVector& data) :
        space_(space), PrintProgress_(PrintProgress), data_(data),
	visitedlistpool(nullptr), enterpoint_(nullptr), data_level0_memory_(nullptr), linkLists_(nullptr), fstdistfunc_(nullptr)
	{}
	
    void checkList1(vector<HnswNode*> list) {
        
        int ok = 1;
        for (size_t i = 0; i < list.size(); i++) {
            for (size_t j = 0; j < list[i]->allFriends[0].size(); j++) {
                for (size_t k = j+1; k < list[i]->allFriends[0].size(); k++) {
                    if (list[i]->allFriends[0][j].second == list[i]->allFriends[0][k].second) {
                        cout << "\nDuplicate links\n\n\n\n\n!!!!!";
                        ok = 0;
                    }
                }
                if (list[i]->allFriends[0][j].second == list[i]) {
                    cout << "\nLink to the same element\n\n\n\n\n!!!!!";
                    ok = 0;
                }
            }
            
        }
        if (ok)
            cout << "\nOK\n";
        else
            cout << "\nNOT OK!!!\n";
        return;
    }

    void getDegreeDistr(string filename, vector<HnswNode*> list) {        
        ofstream out(filename);
        size_t maxdegree = 0;
        for (HnswNode* node : list) {
            if (node->allFriends[0].size() > maxdegree)
                maxdegree = node->allFriends[0].size();
        }
        
 
        vector<int> distrin = vector<int>(1000);
        vector<int> distrout = vector<int>(1000);
        vector<int> inconnections = vector<int>(list.size());
        vector<int> outconnections = vector<int>(list.size());
        for (size_t i = 0; i < list.size();i++) {
            for (pair<float, HnswNode*> node : list[i]->allFriends[0]) {
                outconnections[list[i]->getId()]++;
                inconnections[node.second->getId()]++;
            }            
        }
        
        for (size_t i = 0; i < list.size(); i++) {
            distrin[inconnections[i]]++;
            distrout[outconnections[i]]++;            
        }
        
        for (size_t i = 0; i < distrin.size(); i++) {
            out<<i<<"\t"<<distrin[i]<<"\t"<< distrout[i]<<"\n";
        }
        out.close();
        return;
    }
    template <typename dist_t> void Hnsw<dist_t>::CreateIndex(const AnyParams& IndexParams)
    {

        AnyParamManager pmgr(IndexParams);

        generator = new std::default_random_engine(100);

        pmgr.GetParamOptional("M", M_, 16);

        // Let's use a generic algorithm by default!
        pmgr.GetParamOptional("searchMethod", searchMethod_, 0); // this is just to prevent terminating the program when searchMethod is specified
        searchMethod_ = 0;

#ifdef _OPENMP
        indexThreadQty_ = omp_get_max_threads();
#endif
        pmgr.GetParamOptional("indexThreadQty", indexThreadQty_, indexThreadQty_);
        //indexThreadQty_ = 1;
        pmgr.GetParamOptional("efConstruction", efConstruction_, 200);
        pmgr.GetParamOptional("maxM", maxM_, M_);
        pmgr.GetParamOptional("maxM0", maxM0_, M_ * 2);
        pmgr.GetParamOptional("mult", mult_, 1 / log(1.0*M_));
        pmgr.GetParamOptional("delaunay_type", delaunay_type_, 2);
        int post_;
        pmgr.GetParamOptional("post", post_, 0);
        int skip_optimized_index = 0;
        pmgr.GetParamOptional("skip_optimized_index", skip_optimized_index, 0);
        
        LOG(LIB_INFO) << "M                   = " << M_;
        LOG(LIB_INFO) << "indexThreadQty      = " << indexThreadQty_;
        LOG(LIB_INFO) << "efConstruction      = " << efConstruction_;
        LOG(LIB_INFO) << "maxM			          = " << maxM_;
        LOG(LIB_INFO) << "maxM0			          = " << maxM0_;

        LOG(LIB_INFO) << "mult                = " << mult_;
        LOG(LIB_INFO) << "skip_optimized_index= " << skip_optimized_index;
        LOG(LIB_INFO) << "delaunay_type       = "  << delaunay_type_;

		    SetQueryTimeParams(getEmptyParams());
        
        if (data_.empty()) {
          pmgr.CheckUnused();
          return;
        }
        ElList_.resize(data_.size());
        // One entry should be added before all the threads are started, or else add() will not work properly
        HnswNode *first = new HnswNode(data_[0], 0 /* id == 0 */);        
        int level = data_[0]->label();
        if (level < 0) {
            level = getRandomLevel(mult_);
        }
        first->init(level, maxM_, maxM0_);
        maxlevel_ = first->level;
        enterpoint_ = first;
        ElList_[0] = first;

        visitedlistpool = new VisitedListPool(indexThreadQty_, data_.size());

        unique_ptr<ProgressDisplay> progress_bar(PrintProgress_ ? new ProgressDisplay(data_.size(), cerr) : NULL);

#pragma omp parallel for schedule(dynamic,128) num_threads(indexThreadQty_)
        for (int id = 1; id < data_.size(); ++id) {
            HnswNode* node = new HnswNode(data_[id], id);
            add(&space_, node); 
            ElList_[id] = node;
            if (progress_bar) ++(*progress_bar);
        }
        endl(cerr);

        
        if (post_ == 1 || post_ == 2) {
            
            vector <HnswNode *> temp;
            temp.swap(ElList_);
            ElList_.resize(data_.size());
            first = new HnswNode(data_[0], 0 /* id == 0 */);
            int level = data_[0]->label();
            if (level < 0) {
                level = getRandomLevel(mult_);
            }
            first->init(level, maxM_, maxM0_);
            maxlevel_ = first->level;
            enterpoint_ = first;
            ElList_[0] = first;
            /// Making the same index in reverse order 
            unique_ptr<ProgressDisplay> progress_bar1(PrintProgress_ ? new ProgressDisplay(data_.size(), cerr) : NULL);
#pragma omp parallel for schedule(dynamic,128) num_threads(indexThreadQty_)
            for (int id = data_.size() - 1; id >= 1; id--) {
                HnswNode* node = new HnswNode(data_[id], id);
                add(&space_, node);
                ElList_[id] = node;
                if (progress_bar1) ++(*progress_bar1);
            }
            endl(cerr);
            int maxF = 0;

            //int degrees[100] = {0};
#pragma omp parallel for schedule(dynamic,128) num_threads(indexThreadQty_)
            for (int id = 1; id < data_.size(); ++id) {
                HnswNode* node1 = ElList_[id];
                HnswNode* node2 = temp[id];
                vector<pair<float, HnswNode *>> f1 = node1->getAllFriends(0);
                vector<pair<float, HnswNode *>> f2 = node2->getAllFriends(0);
                unordered_set<size_t> intersect = unordered_set<size_t>();
                for (pair<float, HnswNode *>cur : f1) {
                    intersect.insert(cur.second->getId());
                }
                for (pair<float, HnswNode *>cur : f2) {
                    intersect.insert(cur.second->getId());
                }
                if (intersect.size() > maxF)
                    maxF = intersect.size();
                vector<pair<float, HnswNode *>> rez = vector<pair<float, HnswNode *>>();
                
                if (post_ == 2) {
                    priority_queue<HnswNodeDistCloser<dist_t>> resultSet;
                    for (int cur : intersect) {
                        resultSet.emplace(space_.IndexTimeDistance(ElList_[cur]->getData(), ElList_[id]->getData()), ElList_[cur]);
                    }

                    switch (delaunay_type_) {
                    case 0:
                        while (resultSet.size() > maxM0_)
                            resultSet.pop();
                        break;
                    case 2:
                    case 1:
                        ElList_[id]->getNeighborsByHeuristic1(resultSet, maxM0_, &space_);
                        break;
                    case 3:
                        ElList_[id]->getNeighborsByHeuristic3(resultSet, maxM0_, &space_, 0);
                        break;
                    }                    
                    while (!resultSet.empty()) {
                        rez.push_back(make_pair(resultSet.top().getDistance(), resultSet.top().getMSWNodeHier()));
                        resultSet.pop();
                    }
                }
                else 
                if (post_ == 1) {
                    maxM0_ = maxF;

                    for (int cur : intersect) {
                        //NOTE: distance is dummy
                        rez.push_back(make_pair(0.0, ElList_[cur]));
                    }
                }

                ElList_[id]->allFriends[0].swap(rez);
                //degrees[ElList_[id]->allFriends[0].size()]++;
            }
            for (int i = 0; i < temp.size(); i++)
                delete temp[i];
            temp.clear();
           

        }
        // Uncomment for debug mode
        //checkList1(ElList_);
        
        data_level0_memory_ = NULL;
        linkLists_ = NULL;

        if (skip_optimized_index) {
          LOG(LIB_INFO) << "searchMethod			  = " << searchMethod_;
          //feedback index level to label area
          for (int i = 0; i < ElList_.size(); i++) {
              *(ElList_[i]->getData()->label_ptr()) = ElList_[i]->level;
          }
          pmgr.CheckUnused();
          return;
        }

        int friendsSectionSize = (maxM0_ + 1)*sizeof(int);

        //Checking for maximum size of the datasection:
        int dataSectionSize = 1;
        for (int i = 0; i < ElList_.size(); i++) {            
            if (ElList_[i]->getData()->bufferlength()>dataSectionSize)
                dataSectionSize = ElList_[i]->getData()->bufferlength();
        }

        // Selecting custom made functions 
        if (space_.StrDesc().compare("SpaceLp: p = 2 do we have a special implementation for this p? : 1") == 0 && sizeof(dist_t) == 4)
        {
            LOG(LIB_INFO) << "\nThe space is Euclidean";
            vectorlength_ = ((dataSectionSize - 16) >> 2);
            LOG(LIB_INFO) << "Vector length=" << vectorlength_;
            if (vectorlength_ % 16 == 0) {
                LOG(LIB_INFO) << "Thus using an optimised function for base 16";
                fstdistfunc_ = L2SqrSIMD16Ext;
                dist_func_type_ = 1;
                searchMethod_ = 3;
            }
            else {
                LOG(LIB_INFO) << "Thus using function with any base";
                fstdistfunc_ = L2SqrSIMDExt;
                dist_func_type_ = 2;
                searchMethod_ = 3;
            }
        }
        else if (space_.StrDesc().compare("CosineSimilarity") == 0 && sizeof(dist_t) == 4)
        {
            LOG(LIB_INFO) << "\nThe vectorspace is Cosine Similarity";
            vectorlength_ = ((dataSectionSize - 16) >> 2);
            LOG(LIB_INFO) << "Vector length=" << vectorlength_;
            iscosine_ = true;
            if (vectorlength_ % 4 == 0) {
                LOG(LIB_INFO) << "Thus using an optimised function for base 4";
                fstdistfunc_ = NormScalarProductSIMD;
                dist_func_type_ = 3;
                searchMethod_ = 4;
            }
            else {
                LOG(LIB_INFO) << "Thus using function with any base";
                LOG(LIB_INFO) << "Search method 4 is not allowed in this case";
                fstdistfunc_ = NormScalarProductSIMD;
                dist_func_type_ = 3;
                searchMethod_ = 3;
            }
        }
        else {
            LOG(LIB_INFO) << "No appropriate custom distance function for " << space_.StrDesc();
            //if (searchMethod_ != 0 && searchMethod_ != 1)
                searchMethod_ = 0;
            LOG(LIB_INFO) << "searchMethod			  = " << searchMethod_;
            //feedback index level to label area
            for (int i = 0; i < ElList_.size(); i++) {
                *(ElList_[i]->getData()->label_ptr()) = ElList_[i]->level;
            }
            pmgr.CheckUnused();
            return; // No optimized index
        }
        pmgr.CheckUnused();
        LOG(LIB_INFO) << "searchMethod			  = " << searchMethod_;
        memoryPerObject_ = ((dataSectionSize + friendsSectionSize + 15) >> 4) * 16;

        size_t total_memory_allocated = (memoryPerObject_*ElList_.size());
        data_level0_memory_ = (char*)malloc(memoryPerObject_*ElList_.size());        

        offsetLevel0_ = dataSectionSize;
        offsetData_ = 0;
       

        memset(data_level0_memory_, 1, memoryPerObject_*ElList_.size());
        LOG(LIB_INFO) << "Making optimized index";
        data_rearranged_.resize(ElList_.size());
        for (long i = 0; i < ElList_.size(); i++) {            
            ElList_[i]->copyDataAndLevel0LinksToOptIndex(data_level0_memory_ + (size_t)i*memoryPerObject_, offsetLevel0_, offsetData_);
            data_rearranged_[i] = new Object(data_level0_memory_ + (i)*memoryPerObject_ + offsetData_);
            delete ElList_[i]->getData();
        };
        ////////////////////////////////////////////////////////////////////////
        //
        // The next step is needed only fos cosine similarity space
        // All vectors are normalized, so we don't have to normalize them later
        //
        ////////////////////////////////////////////////////////////////////////
        if (iscosine_)
        {
#pragma omp parallel for schedule(dynamic,128) num_threads(indexThreadQty_)
            for (long i = unoptimized_size_; i < ElList_.size(); i++) {
                float *v = (float *)(data_level0_memory_ + (size_t)i*memoryPerObject_ + offsetData_ + 16);
                float sum = 0;
                for (int i = 0; i < vectorlength_; i++) {
                    sum += v[i] * v[i];
                }                
                if (sum != 0.0) {
                    sum = 1 / sqrt(sum);
                    for (int i = 0; i < vectorlength_; i++) {
                        v[i] *= sum;
                    }
                }
            };
        }      
        
        /////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////
        linkLists_ = (char**)malloc(sizeof(void*)*ElList_.size());
        for (long i = 0; i < ElList_.size(); i++) {
            //reserve level also in optimized index
            *(data_rearranged_[i]->label_ptr()) = ElList_[i]->level;
            if (ElList_[i]->level < 1) {
                linkLists_[i] = nullptr;
                continue;
            }
            // TODO Can this one overflow? I really doubt
            SIZEMASS_TYPE sizemass = ((ElList_[i]->level)*(maxM_ + 1))*sizeof(int);
            total_memory_allocated += sizemass;
            char *linkList = (char*)malloc(sizemass);
            linkLists_[i] = linkList;
            ElList_[i]->copyHigherLevelLinksToOptIndex(linkList, 0);
        };
        enterpointId_ = enterpoint_->getId();
        LOG(LIB_INFO) << "Finished making optimized index";
        LOG(LIB_INFO) << "Maximum level = " << enterpoint_->level;
        LOG(LIB_INFO) << "Total memory allocated for optimized index+data: " << (total_memory_allocated >> 20) << " Mb";

        //level was already copied to data_rearranged_[]->label_ptr(). So, we can delete ElList_ here.
        for (int i = 0; i < ElList_.size(); i++)
            delete ElList_[i];
        ElList_.clear();

        if (unoptimized_size_ > 0) {
            free(data_level0_memory_unoptimized_);
            unoptimized_size_ = 0;
            data_level0_memory_unoptimized_ = nullptr;
        }
    }
    template <typename dist_t> void Hnsw<dist_t>::UnoptimizeIndex(ObjectVector& data) {
        //data_level0_memory_          : rearranged to data_. we should keep the memory to keep the actual vector data
        //data_rearranged_, linkLists_ : rearranged to ElList_, .allFriends and initialize them
        //enterpointId_                : enterpoint_
        if (data_rearranged_.empty()) {
            LOG(LIB_INFO) << "The index seems not optimized.";
            return;
        }
        if (dist_func_type_ != 3) {
            LOG(LIB_INFO) << "Only for CosineSimilarity optimized index.";
            return;
        }

#ifdef _OPENMP
        indexThreadQty_ = omp_get_max_threads();
#endif

        data.clear();
        ElList_.clear();
        data.reserve(data_rearranged_.size());

        int new_size = 0;
        vector<int> new_id;
        new_id.resize(data_rearranged_.size());
        int new_enterpointId = 0;
        int max_level = 0;
        for (int i = 0; i < data_rearranged_.size(); i++) {
            int level = data_rearranged_[i]->label();
            if (level > -1) {
                data.resize(new_size + 1);
                data[new_size] = new Object((char *)data_rearranged_[i]->buffer());
                new_id[i] = new_size;
                *(data[new_size]->id_ptr()) = new_size; //set new id
                if (level > max_level) {
                    max_level = level;
                    new_enterpointId = new_size;
                }
                new_size++;
            } else {
                // Skip disabled objects
                new_id[i] = -1;
            }
        }

        ElList_.resize(new_size);
        for (int id = 0; id < new_id.size(); ++id) {
            if (new_id[id] > -1) {
                HnswNode* node = new HnswNode(data_[new_id[id]], new_id[id]);
                int level = data_[new_id[id]]->label();
                //TODO: specify the maxM and maxM0 by Params or current values
                node->init(level, 100, 200); //FIXME: Fixed value....
                ElList_[new_id[id]] = node;
            }
        }
        enterpoint_ = ElList_[new_enterpointId];
        maxlevel_ = max_level;

        LOG(LIB_INFO) << "Unoptimizing index.";
        unique_ptr<ProgressDisplay> progress_bar(PrintProgress_ ? new ProgressDisplay(new_id.size(), cerr) : NULL);
#pragma omp parallel for schedule(dynamic,128) num_threads(indexThreadQty_)
        for (int id = 0; id < new_id.size(); ++id) {
            if (new_id[id] > -1) {
                int level = data_[new_id[id]]->label();
                size_t qty = data_[new_id[id]]->datalength() >> 2;
                for (int i = 0; i <= level; i++) {
                    int  *data;
                    if (i > 0) {
                        data = (int *)(linkLists_[id] + (maxM_ + 1)*(i - 1)*sizeof(int));
                    } else {
                        data = (int *)(data_level0_memory_ + id*memoryPerObject_ + offsetLevel0_);
                    }
                    int size = *data;
                    for (int j = 1; j <= size; j++) {
                        int tnum = *(data + j);
                        if (new_id[tnum] > -1) {
                            //TODO: support for the other dist_func_type_ than 3(CosineSimilarity).
                            //Assumed all vectors in the index were already normalized
                            dist_t d = (ScalarProductSIMD((float *)(data_level0_memory_ + id*memoryPerObject_ + offsetData_ + 16),
                                                          (float *)(data_level0_memory_ + tnum*memoryPerObject_ + offsetData_ + 16), qty));
                            ElList_[new_id[id]]->addFriendlevel(i, d, ElList_[new_id[tnum]], &space_, delaunay_type_);
                        }
                    }
                }
            }
            if (progress_bar) ++(*progress_bar);
        }
        endl(cerr);

        LOG(LIB_INFO) << "Checking and fixing too much eaten by disableDataPoint().";
        //TODO: specify it by Params
        efConstruction_ = 1000; //FIXME: Fixed value....
        M_ = maxM_; //needs to be recovered, because not specified yet.
        unique_ptr<ProgressDisplay> progress_bar2(PrintProgress_ ? new ProgressDisplay(new_id.size(), cerr) : NULL);
#pragma omp parallel for schedule(dynamic,128) num_threads(indexThreadQty_)
        for (int id = 0; id < new_id.size(); ++id) {
            if (new_id[id] > -1) {
                int level = data_[new_id[id]]->label();
                bool needs_readd = false;
                for (int i = 0; i <= level; i++) {
                    int  *data;
                    if (i > 0) {
                        data = (int *)(linkLists_[id] + (maxM_ + 1)*(i - 1)*sizeof(int));
                    } else {
                        data = (int *)(data_level0_memory_ + id*memoryPerObject_ + offsetLevel0_);
                    }
                    int size = *data;
                    int new_size = ElList_[new_id[id]]->allFriends[i].size();
                    if (size == 0 || new_size * 3 < size) {
                        needs_readd = true;
                    }
                }
                if (needs_readd) {
                    add(&space_, ElList_[new_id[id]]);
                }
            }
            if (progress_bar2) ++(*progress_bar2);
        }
        endl(cerr);

        //NOTE: memory at data_level0_memory_ is kept because it has Objects of data_ now.
        data_level0_memory_unoptimized_ = data_level0_memory_;
        unoptimized_size_ = data_.size();
        data_level0_memory_ = NULL;

        for (int i = 0; i < data_rearranged_.size(); i++) {
            if (linkLists_[i])
                free(linkLists_[i]);
        }
        free(linkLists_);
        linkLists_ = NULL;

        for (const Object* p : data_rearranged_) delete p;
        data_rearranged_.clear();

        searchMethod_ = 0;
        fstdistfunc_ = nullptr;
        dist_func_type_ = 0;

        LOG(LIB_INFO) << "Finished unoptimizing index. (" << unoptimized_size_ << " entries)";
    }

    template <typename dist_t>
    void Hnsw<dist_t>::SetQueryTimeParams(const AnyParams& QueryTimeParams) {
        AnyParamManager pmgr(QueryTimeParams);

        if (pmgr.hasParam("ef") && pmgr.hasParam("efSearch")) {
          throw new runtime_error("The user shouldn't specify parameters ef and efSearch at the same time (they are synonyms)");
        }

        // ef and efSearch are going to be parameter-synonyms with the default value 20
        pmgr.GetParamOptional("ef", ef_, 20);
        pmgr.GetParamOptional("efSearch", ef_, ef_);

        int tmp;
        pmgr.GetParamOptional("searchMethod", tmp, 0); // this is just to prevent terminating the program when searchMethod is specified

        string tmps;
        pmgr.GetParamOptional("algoType", tmps, "hybrid");
        ToLower(tmps);
        if (tmps == "v1merge") searchAlgoType_ = kV1Merge;
        else if (tmps == "old") searchAlgoType_ = kOld;
        else if (tmps == "hybrid") searchAlgoType_ = kHybrid;
        else {
          throw runtime_error("algoType should be one of the following: old, v1merge");
        }

        pmgr.CheckUnused();
        LOG(LIB_INFO) << "Set HNSW query-time parameters:";
        LOG(LIB_INFO) << "ef(Search)         =" << ef_;
        LOG(LIB_INFO) << "algoType           =" << searchAlgoType_;
    }

    template <typename dist_t>
    const Object* Hnsw<dist_t>::GetInternalObject(IdType objId) {
        if (objId < 0 || objId >= data_rearranged_.size()) {
            return nullptr;
        }
        return data_rearranged_[objId];
    }

    template <typename dist_t>
    const std::string Hnsw<dist_t>::StrDesc() const {
        return METH_HNSW + ConvertToString(searchMethod_);
    }

    template <typename dist_t>
    Hnsw<dist_t>::~Hnsw() {
        
        delete visitedlistpool;
        if (data_level0_memory_)
            free(data_level0_memory_);
        if (linkLists_) {
            for (int i = 0; i < data_rearranged_.size(); i++) {
                if (linkLists_[i])
                    free(linkLists_[i]);
            }
            free(linkLists_);
        }
        for (int i = 0; i < ElList_.size(); i++)
            delete ElList_[i];
        for (const Object* p : data_rearranged_) delete p;
    }
    
    template <typename dist_t>
     void Hnsw<dist_t>::add(const Space<dist_t>* space, HnswNode *NewElement) {
        int curlevel = NewElement->getData()->label();
        if (curlevel < 0) {
            curlevel = getRandomLevel(mult_);
        }
        unique_lock<mutex> *lock = nullptr;
        if (curlevel > maxlevel_)
            lock = new unique_lock<mutex>(MaxLevelGuard_);

        NewElement->init(curlevel, maxM_, maxM0_);       

        int maxlevelcopy = maxlevel_;
        HnswNode *ep = enterpoint_;
        if (curlevel < maxlevelcopy) {
            const Object* currObj = ep->getData();

            dist_t d = space->IndexTimeDistance(NewElement->getData(), currObj);
            dist_t curdist = d;
            HnswNode *curNode = ep;
            for (int level = maxlevelcopy; level > curlevel; level--) {
                bool changed = true;
                while (changed) {
                    changed = false;
                    unique_lock<mutex>  lock(curNode->accessGuard_);
                    const vector<pair<float, HnswNode*>>& neighbor = curNode->getAllFriends(level);
                    int size = neighbor.size();
                    for (int i = 0; i < size; i++)
                    {
                        if (i + 1 < size) {
                            HnswNode *node = neighbor[i + 1].second;
                            _mm_prefetch((char *)(node->getData()), _MM_HINT_T0);
                        }
                        if (i + 2 < size) {
                            HnswNode *node = neighbor[i + 2].second;
                            _mm_prefetch((char *)node, _MM_HINT_T0);
                        }

                        currObj = (neighbor[i].second)->getData();
                        d = space->IndexTimeDistance(NewElement->getData(), currObj);
                        if (d < curdist) {
                            curdist = d;
                            curNode = neighbor[i].second;
                            changed = true;
                        }
                    }

                }

            }
            ep = curNode;
        }

        
        for (int level = min(curlevel, maxlevelcopy); level >= 0; level--) {
			priority_queue<HnswNodeDistCloser<dist_t>> resultSet;
            kSearchElementsWithAttemptsLevel(space, NewElement->getData(), efConstruction_, resultSet, ep, level);
           
            switch (delaunay_type_) {
            case 0:
                while (resultSet.size() > M_)
                    resultSet.pop();
                break;
            case 1:
                NewElement->getNeighborsByHeuristic1(resultSet, M_, space);
                break;
            case 2:
                NewElement->getNeighborsByHeuristic2(resultSet, M_, space, level);
                break;
            case 3:
                NewElement->getNeighborsByHeuristic3(resultSet, M_, space, level);
                break;
            }
            while (!resultSet.empty()) {
                link(resultSet.top().getDistance(), resultSet.top().getMSWNodeHier(), NewElement, level, space, delaunay_type_);
                resultSet.pop();
            }

        }
        if (curlevel > enterpoint_->level)
        {
            enterpoint_ = NewElement;
            maxlevel_ = curlevel;
        }
        if (lock != nullptr)
            delete lock;
    }




    template <typename dist_t>
    void
        Hnsw<dist_t>::kSearchElementsWithAttemptsLevel(const Space<dist_t>* space,
            const Object* queryObj, size_t efConstruction, priority_queue<HnswNodeDistCloser<dist_t>>& resultSet, HnswNode* ep, int level) const
    {
#if EXTEND_USE_EXTENDED_NEIGHB_AT_CONSTR!=0
        priority_queue<HnswNodeDistCloser<dist_t>> fullResultSet;
#endif
        
#if USE_BITSET_FOR_INDEXING
        VisitedList * vl = visitedlistpool->getFreeVisitedList();
        vl_type *mass = vl->mass;
        vl_type curV = vl->curV;
#else
        unordered_set<HnswNode*>             visited;
#endif
        HnswNode* provider = ep;
        priority_queue <HnswNodeDistFarther<dist_t>>   candidateSet;
        dist_t d = space->IndexTimeDistance(queryObj, provider->getData());
        HnswNodeDistFarther<dist_t> ev(d, provider);

        candidateSet.push(ev);
        resultSet.emplace(d, provider);
        
#if EXTEND_USE_EXTENDED_NEIGHB_AT_CONSTR!=0
        fullResultSet.emplace(d, provider);
#endif
        
#if USE_BITSET_FOR_INDEXING
        size_t nodeId = provider->getId();
        mass[nodeId] = curV;
#else
        visited.insert(provider);
#endif


        while (!candidateSet.empty()) {
            const HnswNodeDistFarther<dist_t>& currEv = candidateSet.top();
            dist_t lowerBound = resultSet.top().getDistance();

            /*
            * Check if we reached a local minimum.
            */
            if (currEv.getDistance() > lowerBound) {
                break;
            }
            HnswNode* currNode = currEv.getMSWNodeHier();

            /*
            * This lock protects currNode from being modified
            * while we are accessing elements of currNode.
            */
            unique_lock<mutex>  lock(currNode->accessGuard_);
            const vector<pair<float, HnswNode*>>& neighbor = currNode->getAllFriends(level);

            // Can't access curEv anymore! The reference would become invalid
            candidateSet.pop();

            // calculate distance to each neighbor
            for (auto iter = neighbor.begin(); iter != neighbor.end(); ++iter) {
#if USE_BITSET_FOR_INDEXING
                size_t nodeId = (*iter).second->getId();
                if (mass[nodeId] != curV)
#else
                if (visited.find((*iter).second) == visited.end())
#endif
                    _mm_prefetch((char *)(*iter).second->getData(), _MM_HINT_T0);
            }

            for (auto iter = neighbor.begin(); iter != neighbor.end(); ++iter) {
#if USE_BITSET_FOR_INDEXING
                size_t nodeId = (*iter).second->getId();
                if (mass[nodeId] != curV) {
                    mass[nodeId] = curV;
#else
                if (visited.find((*iter).second) == visited.end()) {
                    visited.insert((*iter).second);
#endif
                    d = space->IndexTimeDistance(queryObj, (*iter).second->getData());
                    HnswNodeDistFarther<dist_t> evE1(d, (*iter).second);
        
#if EXTEND_USE_EXTENDED_NEIGHB_AT_CONSTR!=0
                    fullResultSet.emplace(d, (*iter).second);
#endif
        
                    if (resultSet.size() < efConstruction || resultSet.top().getDistance() > d) {
                        resultSet.emplace(d, (*iter).second);
                        candidateSet.push(evE1);
                        if (resultSet.size() > efConstruction) {
                            resultSet.pop();
                        }
                    }
                }
            }
        }
        
#if EXTEND_USE_EXTENDED_NEIGHB_AT_CONSTR!=0
        resultSet.swap(fullResultSet);
#endif
        
#if USE_BITSET_FOR_INDEXING
        visitedlistpool->releaseVisitedList(vl);
#endif
    }

    template <typename dist_t>
    void Hnsw<dist_t>::addToElementListSynchronized(HnswNode *HierElement) {
        unique_lock<mutex> lock(ElListGuard_);
        ElList_.push_back(HierElement);
    }
    template <typename dist_t>
    void Hnsw<dist_t>::Search(RangeQuery<dist_t>* query, IdType) const {
        throw runtime_error("Range search is not supported!");
    }


    template <typename dist_t>
    void Hnsw<dist_t>::Search(KNNQuery<dist_t>* query, IdType startObj) const  {
        bool useOld = searchAlgoType_ == kOld || (searchAlgoType_ == kHybrid && ef_ >= 1000);
        //cout << "Ef = " << ef_ << " use old = " << useOld << endl;
        switch (searchMethod_) {
                default:
                  throw runtime_error("Invalid searchMethod: " + ConvertToString(searchMethod_));
                  break;
            		case 0:
                        /// Basic search using Nmslib data structure:
                      if (useOld)
                        const_cast<Hnsw *>(this)->baseSearchAlgorithmOld(query);
                      else
                        const_cast<Hnsw *>(this)->baseSearchAlgorithmV1Merge(query);
            			break;
            		case 1:
                        /// Experimental search using Nmslib data structure (should not be used):
                        const_cast<Hnsw*>(this)->listPassingModifiedAlgorithm(query);
            			break;
            		case 3:
                        /// Basic search using optimized index(cosine+L2)
                        if (useOld)
                          const_cast<Hnsw*>(this)->SearchL2CustomOld(query);
                        else
                          const_cast<Hnsw *>(this)->SearchL2CustomV1Merge(query, startObj);
            			break;
            		case 4:
                        /// Basic search using optimized index with one-time normalized cosine similarity
                        /// Only for cosine similarity!
                        if (useOld)
                          const_cast<Hnsw *>(this)->SearchCosineNormalizedOld(query);
                        else
                          const_cast<Hnsw *>(this)->SearchCosineNormalizedV1Merge(query, startObj);
                        break;
            		};
    }

    template <typename dist_t>
    void Hnsw<dist_t>::SaveIndex(const string &location) {
        if (!data_level0_memory_)
        {
            LOG(LIB_INFO) << "Trying to output the storing non-optimized index as optimized...";
            long dist_func_type_reserve = dist_func_type_;
            size_t searchMethod_reserve = searchMethod_;
            size_t memoryPerObject_reserve = memoryPerObject_;

            std::ofstream output(location, std::ios::binary);

            int friendsSectionSize = (maxM0_ + 1)*sizeof(int);

            //Checking for maximum size of the datasection:
            int dataSectionSize = 1;
            for (int i = 0; i < ElList_.size(); i++) {
                if (ElList_[i]->getData()->bufferlength()>dataSectionSize)
                    dataSectionSize = ElList_[i]->getData()->bufferlength();
            }

            // Selecting custom made functions
            if (space_.StrDesc().compare("SpaceLp: p = 2 do we have a special implementation for this p? : 1") == 0 && sizeof(dist_t) == 4)
            {
                LOG(LIB_INFO) << "\nThe space is Euclidean";
                vectorlength_ = ((dataSectionSize - 16) >> 2);
                LOG(LIB_INFO) << "Vector length=" << vectorlength_;
                if (vectorlength_ % 16 == 0) {
                    LOG(LIB_INFO) << "Thus using an optimised function for base 16";
                    dist_func_type_ = 1;
                    searchMethod_ = 3;
                }
                else {
                    LOG(LIB_INFO) << "Thus using function with any base";
                    dist_func_type_ = 2;
                    searchMethod_ = 3;
                }
            }
            else if (space_.StrDesc().compare("CosineSimilarity") == 0 && sizeof(dist_t) == 4)
            {
                LOG(LIB_INFO) << "\nThe vectorspace is Cosine Similarity";
                vectorlength_ = ((dataSectionSize - 16) >> 2);
                LOG(LIB_INFO) << "Vector length=" << vectorlength_;
                iscosine_ = true;
                if (vectorlength_ % 4 == 0) {
                    LOG(LIB_INFO) << "Thus using an optimised function for base 4";
                    dist_func_type_ = 3;
                    searchMethod_ = 4;
                }
                else {
                    LOG(LIB_INFO) << "Thus using function with any base";
                    LOG(LIB_INFO) << "Search method 4 is not allowed in this case";
                    dist_func_type_ = 3;
                    searchMethod_ = 3;
                }
            }
            else {
                LOG(LIB_INFO) << "No appropriate custom distance function for " << space_.StrDesc();
                throw runtime_error("Storing non-optimized index is not supported yet!");
            }
            LOG(LIB_INFO) << "searchMethod			  = " << searchMethod_;
            memoryPerObject_ = ((dataSectionSize + friendsSectionSize + 15) >> 4) * 16;

            writeBinaryPOD(output, (unsigned long)ElList_.size());
            writeBinaryPOD(output, memoryPerObject_);
            writeBinaryPOD(output, (size_t)dataSectionSize);
            writeBinaryPOD(output, (size_t)0);
            writeBinaryPOD(output, maxlevel_);
            writeBinaryPOD(output, (unsigned long)enterpoint_->getId());
            writeBinaryPOD(output, maxM_);
            writeBinaryPOD(output, maxM0_);
            writeBinaryPOD(output, dist_func_type_);
            writeBinaryPOD(output, searchMethod_);

            char *tmp_data;
            tmp_data = (char*)malloc(memoryPerObject_);
            const Object* tmp_obj;
            tmp_obj = new Object(tmp_data);
            for (long j = 0; j < ElList_.size(); j++) {
                memset(tmp_data, 1, memoryPerObject_);
                ElList_[j]->copyDataAndLevel0LinksToOptIndex(tmp_data, dataSectionSize, 0);
                *(tmp_obj->label_ptr()) = ElList_[j]->level;
                if (iscosine_ && j >= unoptimized_size_)
                {
                    float *v = (float *)(tmp_data + 16);
                    float sum = 0;
                    for (int i = 0; i < vectorlength_; i++) {
                        sum += v[i] * v[i];
                    }
                    if (sum != 0.0) {
                        sum = 1 / sqrt(sum);
                        for (int i = 0; i < vectorlength_; i++) {
                            v[i] *= sum;
                        }
                    }
                }

                output.write(tmp_data, memoryPerObject_);
            }
            delete tmp_obj;
            free(tmp_data);

            tmp_data = (char*)malloc(((maxlevel_)*(maxM_ + 1))*sizeof(int));
            for (long i = 0; i < ElList_.size(); i++) {
                SIZEMASS_TYPE sizemass = ((ElList_[i]->level)*(maxM_ + 1))*sizeof(int);
                writeBinaryPOD(output, sizemass);
                if((sizemass)) {
                    memset(tmp_data, 2, sizemass);
                    ElList_[i]->copyHigherLevelLinksToOptIndex(tmp_data, 0);
                    output.write(tmp_data, sizemass);
                }
            }
            free(tmp_data);

            output.close();

            //restore values of non-optimized
            dist_func_type_ = dist_func_type_reserve;
            searchMethod_ = searchMethod_reserve;
            memoryPerObject_ = memoryPerObject_reserve;

            return;
        }

        std::ofstream output(location, std::ios::binary);
        streampos position;
        totalElementsStored_ = data_rearranged_.size();

        writeBinaryPOD(output, totalElementsStored_);
        writeBinaryPOD(output, memoryPerObject_);
        writeBinaryPOD(output, offsetLevel0_);
        writeBinaryPOD(output, offsetData_);
        writeBinaryPOD(output, maxlevel_);
        writeBinaryPOD(output, enterpointId_);
        writeBinaryPOD(output, maxM_);
        writeBinaryPOD(output, maxM0_);
        writeBinaryPOD(output, dist_func_type_);
        writeBinaryPOD(output, searchMethod_);

            
            
        size_t data_plus_links0_size = memoryPerObject_*totalElementsStored_;
        LOG(LIB_INFO) << "writing " << data_plus_links0_size << " bytes";
        output.write(data_level0_memory_, data_plus_links0_size);
        
        //output.write(data_level0_memory_, memoryPerObject_*totalElementsStored_);

        //size_t total_memory_allocated = 0;

        for (size_t i = 0; i < totalElementsStored_; i++) {            
            // TODO Can this one overflow? I really doubt
            SIZEMASS_TYPE sizemass = ((data_rearranged_[i]->label())*(maxM_ + 1))*sizeof(int);
            writeBinaryPOD(output, sizemass);
            if((sizemass))
                output.write(linkLists_[i], sizemass);
        };        
        output.close();


    }

    template <typename dist_t>
    void Hnsw<dist_t>::LoadIndex(const string &location) {
        LOG(LIB_INFO) << "Loading index from "<<location;
        std::ifstream input(location, std::ios::binary);
        streampos position;
        
        //input.seekg(0, std::ios::beg);

        readBinaryPOD(input, totalElementsStored_);
        readBinaryPOD(input, memoryPerObject_);
        readBinaryPOD(input, offsetLevel0_);
        readBinaryPOD(input, offsetData_);
        readBinaryPOD(input, maxlevel_);
        readBinaryPOD(input, enterpointId_);
        readBinaryPOD(input, maxM_);
        readBinaryPOD(input, maxM0_);
        readBinaryPOD(input, dist_func_type_);
        readBinaryPOD(input, searchMethod_);

        LOG(LIB_INFO) << "searchMethod: " << searchMethod_;
        
        if (dist_func_type_ == 1)
             fstdistfunc_ = L2SqrSIMD16Ext;
        else if (dist_func_type_ == 2)
            fstdistfunc_ = L2SqrSIMDExt;
        else if (dist_func_type_ == 3)
            fstdistfunc_ = NormScalarProductSIMD;


  
//        LOG(LIB_INFO) << input.tellg();
        LOG(LIB_INFO) << "Total: " << totalElementsStored_<< ", Memory per object: " << memoryPerObject_;
        size_t data_plus_links0_size = memoryPerObject_*totalElementsStored_;
        data_level0_memory_ = (char*)malloc(data_plus_links0_size);        
        input.read(data_level0_memory_, data_plus_links0_size);            
        linkLists_ = (char**)malloc(sizeof(void*)*totalElementsStored_);

        data_rearranged_.resize(totalElementsStored_);

        for (size_t i = 0; i < totalElementsStored_; i++) {
            SIZEMASS_TYPE linkListSize;
            readBinaryPOD(input, linkListSize);
            position = input.tellg();
            if (linkListSize == 0) {
                linkLists_[i] = nullptr;
            }
            else {
                linkLists_[i] = (char *) malloc(linkListSize);
                input.read(linkLists_[i], linkListSize);
            }
            data_rearranged_[i] = new Object(data_level0_memory_ + (i)*memoryPerObject_ + offsetData_);
        }
        LOG(LIB_INFO) << "Finished loading index";
        visitedlistpool = new VisitedListPool(1, totalElementsStored_);
      
        input.close();

    }


    	template <typename dist_t>
    	void Hnsw<dist_t>::baseSearchAlgorithmOld(KNNQuery<dist_t> *query) {
    		VisitedList * vl = visitedlistpool->getFreeVisitedList();
            vl_type *massVisited = vl->mass;
            vl_type currentV = vl->curV;

    		HnswNode* provider;
    		int maxlevel1 = enterpoint_->level;
    		provider = enterpoint_;
    
    		const Object* currObj = provider->getData();
    
    		dist_t d = query->DistanceObjLeft(currObj);
    		dist_t curdist = d;
    		HnswNode *curNode = provider;
    		for (int i = maxlevel1; i > 0; i--) {
    			bool changed = true;
    			while (changed) {
    				changed = false;
    
    				const vector<pair<float, HnswNode*>>& neighbor = curNode->getAllFriends(i);
    				for (auto iter = neighbor.begin(); iter != neighbor.end(); ++iter) {
    					_mm_prefetch((char *)(*iter).second->getData(), _MM_HINT_T0);
    				}
    				for (auto iter = neighbor.begin(); iter != neighbor.end(); ++iter) {
    					currObj = (*iter).second->getData();
    					d = query->DistanceObjLeft(currObj);
    					if (d < curdist) {
    						curdist = d;
    						curNode = (*iter).second;
    						changed = true;
    					}
    				}
    			}
    		}
    	
    
    		priority_queue <HnswNodeDistFarther<dist_t>> candidateQueue; //the set of elements which we can use to evaluate    																			 
    		priority_queue <HnswNodeDistCloser<dist_t>> closestDistQueue1; //The set of closest found elements 
    
    		HnswNodeDistFarther<dist_t> ev(curdist, curNode);
    		candidateQueue.emplace(curdist, curNode);
    		closestDistQueue1.emplace(curdist, curNode);
    
    		query->CheckAndAddToResult(curdist, curNode->getData());
    		massVisited[curNode->getId()] = currentV;
    		//visitedQueue.insert(curNode->getId());
    
    		////////////////////////////////////////////////////////////////////////////////
    		// PHASE TWO OF THE SEARCH
    		// Extraction of the neighborhood to find k nearest neighbors.
    		////////////////////////////////////////////////////////////////////////////////
    		
    		while (!candidateQueue.empty()) {
    			
    			auto iter = candidateQueue.top(); // This one was already compared to the query
    			const HnswNodeDistFarther<dist_t>& currEv = iter;
    			//Check condtion to end the search
    			dist_t lowerBound = closestDistQueue1.top().getDistance();
    			if (currEv.getDistance() > lowerBound) {
    				break;
    			}
    
    			HnswNode *initNode = currEv.getMSWNodeHier();
    			candidateQueue.pop();
    
    			const vector<pair<float, HnswNode*>>& neighbor = (initNode)->getAllFriends(0);
    
    			size_t curId;
    
    			for (auto iter = neighbor.begin(); iter != neighbor.end(); ++iter) {
    				_mm_prefetch((char *)(*iter).second->getData(), _MM_HINT_T0);
    				_mm_prefetch((char *)(massVisited + (*iter).second->getId()), _MM_HINT_T0);
    			}
    			//calculate distance to each neighbor
    			for (auto iter = neighbor.begin(); iter != neighbor.end(); ++iter) {
    
    				curId = (*iter).second->getId();
    				
    				if (!(massVisited[curId] == currentV))
    				{
    					massVisited[curId] = currentV;
    					currObj = (*iter).second->getData();
    					d = query->DistanceObjLeft(currObj);
    					if (closestDistQueue1.top().getDistance() > d || closestDistQueue1.size() < ef_) {
    						{
    							query->CheckAndAddToResult(d, currObj);    
    							candidateQueue.emplace(d, (*iter).second);
    							closestDistQueue1.emplace(d, (*iter).second);
    							if (closestDistQueue1.size() > ef_) {
    								closestDistQueue1.pop();
    							}
    						}
    					}
    				}
    			}
    
    		}
    		visitedlistpool->releaseVisitedList(vl);
    
    	}

template <typename dist_t>
void Hnsw<dist_t>::baseSearchAlgorithmV1Merge(KNNQuery<dist_t> *query) {
  VisitedList * vl = visitedlistpool->getFreeVisitedList();
  vl_type *massVisited = vl->mass;
  vl_type currentV = vl->curV;

  HnswNode* provider;
  int maxlevel1 = enterpoint_->level;
  provider = enterpoint_;

  const Object* currObj = provider->getData();

  dist_t d = query->DistanceObjLeft(currObj);
  dist_t curdist = d;
  HnswNode *curNode = provider;
  for (int i = maxlevel1; i > 0; i--) {
    bool changed = true;
    while (changed) {
      changed = false;

      const vector<pair<float, HnswNode*>>& neighbor = curNode->getAllFriends(i);
      for (auto iter = neighbor.begin(); iter != neighbor.end(); ++iter) {
        _mm_prefetch((char *)(*iter).second->getData(), _MM_HINT_T0);
      }
      for (auto iter = neighbor.begin(); iter != neighbor.end(); ++iter) {
        currObj = (*iter).second->getData();
        d = query->DistanceObjLeft(currObj);
        if (d < curdist) {
          curdist = d;
          curNode = (*iter).second;
          changed = true;
        }
      }
    }
  }


  SortArrBI<dist_t,HnswNode*> sortedArr(max<size_t>(ef_, query->GetK()));
  sortedArr.push_unsorted_grow(curdist, curNode);

  int_fast32_t  currElem = 0;

  typedef typename SortArrBI<dist_t, HnswNode*>::Item  QueueItem;
  vector<QueueItem>& queueData = sortedArr.get_data();
  vector<QueueItem>  itemBuff(16*M_);

  massVisited[curNode->getId()] = currentV;
  //visitedQueue.insert(curNode->getId());

  ////////////////////////////////////////////////////////////////////////////////
  // PHASE TWO OF THE SEARCH
  // Extraction of the neighborhood to find k nearest neighbors.
  ////////////////////////////////////////////////////////////////////////////////

  while(currElem < min(sortedArr.size(),ef_)){
    auto& e = queueData[currElem];
    CHECK(!e.used);
    e.used = true;
    HnswNode* initNode = e.data;
    ++currElem;

    size_t itemQty = 0;
    dist_t topKey = sortedArr.top_key();

    const vector<pair<float, HnswNode*>>& neighbor = (initNode)->getAllFriends(0);

    size_t curId;

    for (auto iter = neighbor.begin(); iter != neighbor.end(); ++iter) {
      _mm_prefetch((char *)(*iter).second->getData(), _MM_HINT_T0);
      _mm_prefetch((char *)(massVisited + (*iter).second->getId()), _MM_HINT_T0);
    }
    //calculate distance to each neighbor
    for (auto iter = neighbor.begin(); iter != neighbor.end(); ++iter) {

      curId = (*iter).second->getId();

      if (!(massVisited[curId] == currentV))
      {
        massVisited[curId] = currentV;
        currObj = (*iter).second->getData();
        d = query->DistanceObjLeft(currObj);

        if (d < topKey || sortedArr.size() < ef_) {
          itemBuff[itemQty++]=QueueItem(d, (*iter).second);
        }
      }
    }

    if (itemQty) {
      _mm_prefetch(const_cast<const char*>(reinterpret_cast<char*>(&itemBuff[0])), _MM_HINT_T0);
      std::sort(itemBuff.begin(), itemBuff.begin() + itemQty);

      size_t insIndex=0;
      if (itemQty > MERGE_BUFFER_ALGO_SWITCH_THRESHOLD) {
        insIndex = sortedArr.merge_with_sorted_items(&itemBuff[0], itemQty);

        if (insIndex < currElem) {
          //LOG(LIB_INFO) << "@@@ " << currElem << " -> " << insIndex;
          currElem = insIndex;
        }
      } else {
        for (size_t ii = 0; ii < itemQty; ++ii) {
          size_t insIndex = sortedArr.push_or_replace_non_empty_exp(itemBuff[ii].key, itemBuff[ii].data);

          if (insIndex < currElem) {
            //LOG(LIB_INFO) << "@@@ " << currElem << " -> " << insIndex;
            currElem = insIndex;
          }
        }
      }
    }
    // To ensure that we either reach the end of the unexplored queue or currElem points to the first unused element
    while (currElem < sortedArr.size() && queueData[currElem].used == true)
      ++currElem;
  }

  for (int_fast32_t i = 0; i < query->GetK() && i < sortedArr.size(); ++i) {
    query->CheckAndAddToResult(queueData[i].key, queueData[i].data->getData());
  }

  visitedlistpool->releaseVisitedList(vl);

}
        // Experimental search algorithm
    	template <typename dist_t>
    	void Hnsw<dist_t>::listPassingModifiedAlgorithm(KNNQuery<dist_t>* query) {
    		int efSearchL = 4; // This parameters defines the confidence of searches at level higher than zero 
                               // for zero level it is set to ef
            //Getting the visitedlist
    		VisitedList * vl = visitedlistpool->getFreeVisitedList();
            vl_type *massVisited = vl->mass;
            vl_type currentV = vl->curV;
    
    		int maxlevel1 = enterpoint_->level;    
    
    		const Object* currObj = enterpoint_->getData();
    
    		dist_t d = query->DistanceObjLeft(currObj);
    		dist_t curdist = d;
    		HnswNode *curNode = enterpoint_;
    
    
    		priority_queue <HnswNodeDistFarther<dist_t>> candidateQueue; //the set of elements which we can use to evaluate    														
    		priority_queue <HnswNodeDistCloser<dist_t>> closestDistQueue= priority_queue <HnswNodeDistCloser<dist_t>>(); //The set of closest found elements 
    		priority_queue <HnswNodeDistCloser<dist_t>> closestDistQueueCpy = priority_queue <HnswNodeDistCloser<dist_t>>();

    		HnswNodeDistFarther<dist_t> ev(curdist, curNode);
    		candidateQueue.emplace(curdist, curNode);
    		closestDistQueue.emplace(curdist, curNode);

    		massVisited[curNode->getId()] = currentV;

    		for (int i = maxlevel1; i > 0; i--) {
    			
    			while (!candidateQueue.empty()) {
    	
    				auto iter = candidateQueue.top(); 
    				const HnswNodeDistFarther<dist_t>& currEv = iter;
    				//Check condtion to end the search
    				dist_t lowerBound = closestDistQueue.top().getDistance();
    				if (currEv.getDistance() > lowerBound) {
    					break;
    				}
    
    				HnswNode *initNode = currEv.getMSWNodeHier();
    				candidateQueue.pop();
    
    				const vector<pair<float, HnswNode*>>& neighbor = (initNode)->getAllFriends(i);
    
    				size_t curId;
    
    				for (auto iter = neighbor.begin(); iter != neighbor.end(); ++iter) {
    					_mm_prefetch((char *)(*iter).second->getData(), _MM_HINT_T0);
    					_mm_prefetch((char *)(massVisited + (*iter).second->getId()), _MM_HINT_T0);
    				}
    				//calculate distance to each neighbor
    				for (auto iter = neighbor.begin(); iter != neighbor.end(); ++iter) {    
    					curId = (*iter).second->getId();
    					if (!(massVisited[curId] == currentV))
    					{
    						massVisited[curId] = currentV;
    						currObj = (*iter).second->getData();
    						d = query->DistanceObjLeft(currObj);
                            if (closestDistQueue.top().getDistance() > d || closestDistQueue.size() < efSearchL) {
                                candidateQueue.emplace(d, (*iter).second);
                                closestDistQueue.emplace(d, (*iter).second);
                                if (closestDistQueue.size() > efSearchL) {
                                    closestDistQueue.pop();
                                }
                            }
    					}
    				}
    
    			}
                //Updating the bitset key:
                currentV++;
                vl->curV++;// not to forget updating in the pool
    			if (currentV == 0) {
    				memset(massVisited, 0, ElList_.size()*sizeof(vl_type));
    				currentV++;
                    vl->curV++;// not to forget updating in the pool
    			}
                candidateQueue = priority_queue <HnswNodeDistFarther<dist_t>>();
                closestDistQueueCpy = priority_queue <HnswNodeDistCloser<dist_t>>(closestDistQueue);
    			if (i > 1) {    // Passing the closest neighbors to layers higher than zero:
                    while (closestDistQueueCpy.size() > 0) {
                        massVisited[closestDistQueueCpy.top().getMSWNodeHier()->getId()] = currentV;
                        candidateQueue.emplace(closestDistQueueCpy.top().getDistance(), closestDistQueueCpy.top().getMSWNodeHier());                        
                        closestDistQueueCpy.pop();
                    }    				
    			}
                else {     // Passing the closest neighbors to the 0 zero layer(one has to add also to query):                    
                    while (closestDistQueueCpy.size() > 0) {
                        massVisited[closestDistQueueCpy.top().getMSWNodeHier()->getId()] = currentV;
                        candidateQueue.emplace(closestDistQueueCpy.top().getDistance(), closestDistQueueCpy.top().getMSWNodeHier());
                        query->CheckAndAddToResult(closestDistQueueCpy.top().getDistance(), closestDistQueueCpy.top().getMSWNodeHier()->getData());
                        closestDistQueueCpy.pop();
                    }
                }
    		}
    		
    		////////////////////////////////////////////////////////////////////////////////
    		// PHASE TWO OF THE SEARCH
    		// Extraction of the neighborhood to find k nearest neighbors.
    		////////////////////////////////////////////////////////////////////////////////
    
    
    		while (!candidateQueue.empty()) {
    
    			auto iter = candidateQueue.top();
    			const HnswNodeDistFarther<dist_t>& currEv = iter;
    			//Check condtion to end the search
    			dist_t lowerBound = closestDistQueue.top().getDistance();
    			if (currEv.getDistance() > lowerBound) {
    				break;
    			}
    
    			HnswNode *initNode = currEv.getMSWNodeHier();
    			candidateQueue.pop();
    
    			const vector<pair<float, HnswNode*>>& neighbor = (initNode)->getAllFriends(0);
    
    			size_t curId;
    
    			for (auto iter = neighbor.begin(); iter != neighbor.end(); ++iter) {
    				_mm_prefetch((char *)(*iter).second->getData(), _MM_HINT_T0);
    				_mm_prefetch((char *)(massVisited + (*iter).second->getId()), _MM_HINT_T0);
    			}
    			//calculate distance to each neighbor
    			for (auto iter = neighbor.begin(); iter != neighbor.end(); ++iter) {
    
    				curId = (*iter).second->getId();
    				if (!(massVisited[curId] == currentV))
    				{
    					massVisited[curId] = currentV;
    					currObj = (*iter).second->getData();
    					d = query->DistanceObjLeft(currObj);
    					if (closestDistQueue.top().getDistance() > d || closestDistQueue.size() < ef_) {
    						{
    							query->CheckAndAddToResult(d, currObj);
    							candidateQueue.emplace(d, (*iter).second);
    							closestDistQueue.emplace(d, (*iter).second);
    							if (closestDistQueue.size() > ef_) {
    								closestDistQueue.pop();
    							}
    						}
    					}
    				}
    			}
    
    		}
    		visitedlistpool->releaseVisitedList(vl);
    
    	}


    template class Hnsw<float>;
    template class Hnsw<double>;
    template class Hnsw<int>;

}
