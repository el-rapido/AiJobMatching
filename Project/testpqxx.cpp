#include <iostream>
#include <pqxx/pqxx>

int main() {
    try {
        pqxx::connection conn("dbname=job_matching user=macos host=localhost");
        if (conn.is_open()) {
            std::cout << "Connected to PostgreSQL!" << std::endl;
        } else {
            std::cout << "Failed to connect." << std::endl;
        }
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    return 0;
}