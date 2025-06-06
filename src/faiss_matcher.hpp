#pragma once
#include <string>
#include <vector>
#include <faiss/IndexIDMap.h>

faiss::IndexIDMap* load_faiss_index(const std::string& path);

void search_top_matches(faiss::IndexIDMap* index, const std::vector<float>& query,
                        int k, std::vector<faiss::idx_t>& ids, std::vector<float>& scores);
