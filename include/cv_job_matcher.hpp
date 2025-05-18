#ifndef CV_JOB_MATCHER_HPP
#define CV_JOB_MATCHER_HPP

#include <string>

// Function to match a CV embedding with jobs from the database
void match_cv_with_jobs(const std::string& cv_embedding_path, 
                       const std::string& db_path,
                       const std::string& faiss_index_path,
                       int top_k);

#endif // CV_JOB_MATCHER_HPP