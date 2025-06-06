#include "faiss_matcher.hpp"
#include <iostream> 
#include <faiss/IndexFlat.h>
#include <faiss/IndexIDMap.h>
#include <faiss/index_io.h>

faiss::IndexIDMap* load_faiss_index(const std::string& path) {
    try {
        faiss::Index* base = faiss::read_index(path.c_str());
        return new faiss::IndexIDMap(base);
    } catch (...) {
        std::cerr << "[FAISS] Failed to load index from " << path << "\n";
        return nullptr;
    }
}

void search_top_matches(faiss::IndexIDMap* index, const std::vector<float>& query,
                        int k, std::vector<faiss::idx_t>& ids, std::vector<float>& scores) {
    ids.resize(k);
    scores.resize(k);
    index->search(1, query.data(), k, scores.data(), ids.data());
}
