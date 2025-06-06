#include "sqlite_helper.hpp"
#include <sqlite3.h>
#include <iostream>

sqlite3* open_database(const std::string& db_path) {
    sqlite3* db;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "[SQLite] Cannot open DB: " << sqlite3_errmsg(db) << "\n";
        return nullptr;
    }
    return db;
}

bool fetch_job_details(sqlite3* db, int job_id, 
                      std::string& title, std::string& description,
                      std::string& location, std::string& source) {
    std::string sql = "SELECT title, description, location, source FROM jobs WHERE id = ?";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQLite] Failed to prepare statement: " << sqlite3_errmsg(db) << "\n";
        return false;
    }
    
    sqlite3_bind_int(stmt, 1, job_id);
    bool success = false;
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        title = sqlite3_column_text(stmt, 0) ? 
               reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)) : "";
        description = sqlite3_column_text(stmt, 1) ? 
                     reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)) : "";
        location = sqlite3_column_text(stmt, 2) ? 
                  reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)) : "";
        source = sqlite3_column_text(stmt, 3) ? 
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)) : "";
        success = true;
    } else {
        std::cerr << "[SQLite] No job found with ID: " << job_id << "\n";
    }
    
    sqlite3_finalize(stmt);
    return success;
}