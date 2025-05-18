#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdlib>
#include <stdexcept>
#include <algorithm>
#include <sqlite3.h>
#include "cv_job_matcher.hpp"

// Configuration constants
const std::string DEFAULT_CV_FILE = "../data/sample_cv.txt";
const std::string DEFAULT_CV_EMBEDDING_OUTPUT = "../output/embedding.json";
const std::string DEFAULT_DB_PATH = "../data/jobs.db";
const std::string DEFAULT_FAISS_INDEX_PATH = "../data/jobs_index.bin";
const int DEFAULT_TOP_K = 3;

void print_usage() {
    std::cout << "Usage: job_matcher [options]\n"
              << "Options:\n"
              << "  --cv-file FILE       Path to CV text file (default: " << DEFAULT_CV_FILE << ")\n"
              << "  --output-file FILE   Path to save embedding output (default: " << DEFAULT_CV_EMBEDDING_OUTPUT << ")\n"
              << "  --db-path FILE       Path to SQLite database (default: " << DEFAULT_DB_PATH << ")\n"
              << "  --index-path FILE    Path to FAISS index file (default: " << DEFAULT_FAISS_INDEX_PATH << ")\n"
              << "  --top-k NUM          Number of top matches to show (default: " << DEFAULT_TOP_K << ")\n"
              << "  --help               Show this help message\n";
}

int main(int argc, char* argv[]) {
    try {
        std::string cv_file = DEFAULT_CV_FILE;
        std::string output_file = DEFAULT_CV_EMBEDDING_OUTPUT;
        std::string db_path = DEFAULT_DB_PATH;
        std::string faiss_index_path = DEFAULT_FAISS_INDEX_PATH;
        int top_k = DEFAULT_TOP_K;
        
        // Parse command line arguments
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            
            if (arg == "--help") {
                print_usage();
                return 0;
            } else if (arg == "--cv-file" && i + 1 < argc) {
                cv_file = argv[++i];
            } else if (arg == "--output-file" && i + 1 < argc) {
                output_file = argv[++i];
            } else if (arg == "--db-path" && i + 1 < argc) {
                db_path = argv[++i];
            } else if (arg == "--index-path" && i + 1 < argc) {
                faiss_index_path = argv[++i];
            } else if (arg == "--top-k" && i + 1 < argc) {
                top_k = std::stoi(argv[++i]);
                if (top_k <= 0) {
                    std::cerr << "Error: top-k must be positive\n";
                    return 1;
                }
            } else if (arg.substr(0, 2) == "--") {
                std::cerr << "Unknown option: " << arg << "\n";
                print_usage();
                return 1;
            }
        }

        std::cout << "\n======================================\n";
        std::cout << "     AI Job Matching System\n";
        std::cout << "======================================\n\n";
        
        std::cout << "[Main] Starting job matching process...\n";
        std::cout << "[Main] CV file: " << cv_file << "\n";
        std::cout << "[Main] Output file: " << output_file << "\n";
        std::cout << "[Main] Database: " << db_path << "\n";
        std::cout << "[Main] FAISS index: " << faiss_index_path << "\n";
        std::cout << "[Main] Top-K matches: " << top_k << "\n";

        // Step 1: Generate embedding for the CV using the Python script
        std::cout << "\n[Main] Step 1: Generating CV embedding using Python script...\n";
        
#ifdef _WIN32
        std::string cmd = "python ..\\src\\embedder.py --file \"" + cv_file + "\" --output \"" + output_file + "\"";
        std::system("mkdir ..\\output 2>nul");
#else
        std::string cmd = "python ../src/embedder.py --file \"" + cv_file + "\" --output \"" + output_file + "\"";
        std::system("mkdir -p ../output");
#endif

        int result = std::system(cmd.c_str());
        if (result != 0) {
            throw std::runtime_error("[Main] Python embedding script failed. Exit code: " + std::to_string(result));
        }
        
        std::cout << "[Main] CV embedding generated successfully.\n";
        
        // Step 2: Match CV with jobs
        std::cout << "\n[Main] Step 2: Matching CV with jobs...\n";
        match_cv_with_jobs(output_file, db_path, faiss_index_path, top_k);
        
        std::cout << "\n[Main] Job matching process completed successfully.\n";
        
    } catch (const std::exception& e) {
        std::cerr << "\n[ERROR] " << e.what() << "\n";
        return 1;
    }
    
    std::cout << "\nPress Enter to exit...";
    std::cin.get();
    return 0;
}