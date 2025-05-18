#pragma once
#include <string>
#include <sqlite3.h>

sqlite3* open_database(const std::string& db_path);

bool fetch_job_details(sqlite3* db, int job_id, 
                      std::string& title, std::string& description,
                      std::string& location, std::string& source);