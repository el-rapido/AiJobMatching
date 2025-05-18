#include "cv_job_matcher.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>
#include <vector>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

using json = nlohmann::json;

struct Job {
    int id;
    std::string title;
    std::string description;
    std::string location;
    std::string source;
    std::vector<std::string> skills;
    float similarity;
};

void match_cv_with_jobs(const std::string& cv_embedding_path, 
                        const std::string& db_path,
                        const std::string& faiss_index_path,
                        int top_k) {
    std::cout << "[CV Job Matcher] Starting CV-Job matching process...\n";
    std::cout << "[CV Job Matcher] Using CV embedding from: " << cv_embedding_path << "\n";
    std::cout << "[CV Job Matcher] Using database: " << db_path << "\n";
    
    // Define output path for matches
    std::string matches_output_path = "../output/matches.json";
    
    // Call the Python script for matching
#ifdef _WIN32
    std::string cmd = "python ..\\src\\job_matcher.py "
                      "--cv-embedding \"" + cv_embedding_path + "\" "
                      "--db-path \"" + db_path + "\" "
                      "--output \"" + matches_output_path + "\" "
                      "--top-k " + std::to_string(top_k);
#else
    std::string cmd = "python ../src/job_matcher.py "
                      "--cv-embedding \"" + cv_embedding_path + "\" "
                      "--db-path \"" + db_path + "\" "
                      "--output \"" + matches_output_path + "\" "
                      "--top-k " + std::to_string(top_k);
#endif

    std::cout << "[CV Job Matcher] Executing command: " << cmd << "\n";
    int result = std::system(cmd.c_str());
    
    if (result != 0) {
        std::cerr << "[CV Job Matcher] Python job matching script failed with exit code: " << result << "\n";
        return;
    }
    
    // Load and display results
    try {
        std::cout << "\n[CV Job Matcher] Loading matching results from " << matches_output_path << "\n";
        std::ifstream file(matches_output_path);
        
        if (!file.is_open()) {
            std::cerr << "[CV Job Matcher] Failed to open matches output file\n";
            return;
        }
        
        json matches_json;
        file >> matches_json;
        file.close();
        
        // Parse jobs from JSON
        std::vector<Job> matches;
        for (const auto& job_json : matches_json) {
            Job job;
            job.id = job_json["id"];
            job.title = job_json["title"];
            job.description = job_json["description"];
            job.location = job_json["location"];
            job.source = job_json["source"];
            job.similarity = job_json["similarity"];
            
            // Parse skills array
            if (job_json.contains("skills") && job_json["skills"].is_array()) {
                for (const auto& skill : job_json["skills"]) {
                    job.skills.push_back(skill);
                }
            }
            
            matches.push_back(job);
        }
        
        // Display results to user
        std::cout << "\n============= Top " << matches.size() << " Job Matches =============\n\n";
        
        for (size_t i = 0; i < matches.size(); i++) {
            const auto& job = matches[i];
            
            std::cout << "Match #" << (i + 1) << " (Similarity: " << job.similarity << ")\n";
            std::cout << "Title: " << job.title << "\n";
            std::cout << "Location: " << job.location << "\n";
            std::cout << "Source: " << job.source << "\n";
            
            std::cout << "Skills: ";
            for (size_t j = 0; j < std::min(job.skills.size(), size_t(5)); j++) {
                std::cout << job.skills[j];
                if (j < std::min(job.skills.size(), size_t(5)) - 1) {
                    std::cout << ", ";
                }
            }
            
            if (job.skills.size() > 5) {
                std::cout << " (+" << (job.skills.size() - 5) << " more)";
            }
            
            std::cout << "\n\n";
            std::cout << "Description Preview: \n";
            
            // Show a preview of the description (first 200 chars)
            if (job.description.length() > 200) {
                std::cout << job.description.substr(0, 200) << "...\n";
            } else {
                std::cout << job.description << "\n";
            }
            
            std::cout << "---------------------------------------------\n\n";
        }
        
        if (matches.empty()) {
            std::cout << "No matching jobs found.\n";
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[CV Job Matcher] Error parsing matches: " << e.what() << "\n";
    }
    
    std::cout << "[CV Job Matcher] Job matching process completed.\n";
}