#ifndef EMBEDDING_HPP
#define EMBEDDING_HPP

#include <string>
#include <vector>

namespace ai {

    class Embedder {
    public:
        explicit Embedder(const std::string& api_key);

        // Generates embedding for the provided text
        std::vector<float> generate_embedding(const std::string& text);

    private:
        std::string api_key_;
        const std::string endpoint_ = "https://api.cohere.ai/v1/embed";
    };

    // Utility: load a file (e.g. CV) into a std::string
    std::string load_file_text(const std::string& file_path);

} // namespace ai

#endif // EMBEDDING_HPP
