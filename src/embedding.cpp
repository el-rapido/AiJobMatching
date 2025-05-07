#include "embedding.hpp"
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

namespace ai {

    Embedder::Embedder(const std::string& api_key) : api_key_(api_key) {}

    std::vector<float> Embedder::generate_embedding(const std::string& text) {
        json payload = {
            {"texts", {text}},
            {"model", "embed-english-v3.0"},
            {"input_type", "search_document"}
        };

        cpr::Response r = cpr::Post(
            cpr::Url{endpoint_},
            cpr::Header{
                {"Authorization", "Bearer " + api_key_},
                {"Content-Type", "application/json"}
            },
            cpr::Body{payload.dump()}
        );

        if (r.status_code != 200) {
            throw std::runtime_error("Embedding API failed: " + r.text);
        }

        json res = json::parse(r.text);
        return res["embeddings"][0].get<std::vector<float>>();
    }

    std::string load_file_text(const std::string& file_path) {
        std::ifstream file(file_path);
        if (!file) throw std::runtime_error("Failed to open: " + file_path);

        std::ostringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

}
