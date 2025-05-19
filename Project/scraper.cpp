#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <optional>
#include <filesystem>
#include <curl/curl.h>
#include <gumbo.h>
#include <nlohmann/json.hpp>

using namespace std::literals;
using json = nlohmann::json;
namespace fs = std::filesystem;

// Configuration structure for each job site
struct SiteConfig {
    std::string name;
    std::string base_url;
    std::string search_url;
    std::string container_tag, container_class;
    std::string title_tag, title_class;
    std::string company_tag, company_class;
    std::string location_tag, location_class;
    std::string salary_tag, salary_class;
    std::string description_tag, description_class;
    std::string date_tag, date_class;
    std::string skills_tag, skills_class;
    std::string pagination_param;
    int max_pages;
    std::chrono::seconds delay{2}; // Configurable delay between requests
};

// REST API configuration for sending scraped data
struct APIConfig {
    std::string endpoint;
    std::string auth_token;
    bool enabled{false};
    
    APIConfig() = default;
    APIConfig(const std::string& endpoint, const std::string& token) 
        : endpoint(endpoint), auth_token(token), enabled(true) {}
};

// Database configuration (optional)
struct DBConfig {
    std::string conn_string;
    bool enabled{false};
    
    DBConfig() = default;
    DBConfig(const std::string& conn_str) : conn_string(conn_str), enabled(true) {}
};

// Output configuration
struct OutputConfig {
    bool json_output{true};
    bool csv_output{true};
    std::string output_dir{"./output"};
    std::chrono::hours scrape_interval{1};
};

// Custom exception for error handling
class ScraperException : public std::runtime_error {
public:
    ScraperException(const std::string& msg) : std::runtime_error(msg) {}
};

// CURL callback function for retrieving web content
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Function to fetch a web page with error handling and retry logic
std::string fetch_page(const std::string& url, int retries = 3) {
    CURL* curl = curl_easy_init();
    if (!curl) throw ScraperException("Failed to initialize CURL");
    
    std::string buffer;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    
    // Add request headers to look more like a browser
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8");
    headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.5");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    
    CURLcode res = CURLE_OK;
    for (int i = 0; i < retries; i++) {
        res = curl_easy_perform(curl);
        if (res == CURLE_OK) break;
        
        std::cerr << "CURL attempt " << (i+1) << " failed: " << curl_easy_strerror(res) << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2 * (i + 1))); // Exponential backoff
    }
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) 
        throw ScraperException(std::string("CURL error after retries: ") + curl_easy_strerror(res));
    
    return buffer;
}

// Function to recursively find nodes in HTML document
void find_nodes(GumboNode* node, const std::string& tag, const std::string& cls, std::vector<GumboNode*>& out) {
    if (!node || node->type != GUMBO_NODE_ELEMENT) return;
    
    GumboElement& e = node->v.element;
    bool match_tag = tag.empty() || (e.tag != GUMBO_TAG_UNKNOWN && 
                                    std::string(gumbo_normalized_tagname(e.tag)) == tag);
    bool match_cls = cls.empty();
    
    if (!cls.empty()) {
        GumboAttribute* a = gumbo_get_attribute(&e.attributes, "class");
        if (a) match_cls = std::string(a->value).find(cls) != std::string::npos;
    }
    
    if (match_tag && match_cls) out.push_back(node);
    
    for (size_t i = 0; i < e.children.length; ++i)
        find_nodes((GumboNode*)e.children.data[i], tag, cls, out);
}

// Function to extract text from a node
std::string extract_text(GumboNode* node) {
    if (!node) return "";
    if (node->type == GUMBO_NODE_TEXT) return node->v.text.text;
    if (node->type != GUMBO_NODE_ELEMENT) return "";
    
    std::string s;
    GumboElement& e = node->v.element;
    for (size_t i = 0; i < e.children.length; ++i) {
        std::string t = extract_text((GumboNode*)e.children.data[i]);
        if (!t.empty()) {
            if (!s.empty()) s += ' ';
            s += t;
        }
    }
    
    return s;
}

// Function to extract an attribute from a node
std::string extract_attr(GumboNode* node, const std::string& name) {
    if (!node || node->type != GUMBO_NODE_ELEMENT) return "";
    GumboAttribute* a = gumbo_get_attribute(&node->v.element.attributes, name.c_str());
    return a ? a->value : std::string();
}

// Function to get the current timestamp in ISO format
std::string now_iso() {
    auto t = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(t);
    std::ostringstream o;
    o << std::put_time(std::localtime(&tt), "%Y-%m-%d %H:%M:%S");
    return o.str();
}

// Function to normalize URL (handle relative URLs)
std::string normalize_url(const std::string& url, const std::string& base_url) {
    if (url.empty()) return "";
    if (url.rfind("http", 0) == 0) return url; // Already absolute
    
    // Handle various relative URL formats
    if (url[0] == '/') {
        // Extract domain from base_url
        size_t protocol_end = base_url.find("://");
        if (protocol_end == std::string::npos) return base_url + url;
        
        size_t domain_start = protocol_end + 3;
        size_t domain_end = base_url.find('/', domain_start);
        if (domain_end == std::string::npos) return base_url + url;
        
        return base_url.substr(0, domain_end) + url;
    }
    
    return base_url + "/" + url;
}

// Function to clean and normalize text content
std::string clean_text(const std::string& text) {
    std::string result;
    bool space = false;
    
    for (char c : text) {
        if (std::isspace(c)) {
            if (!space && !result.empty()) {
                result += ' ';
                space = true;
            }
        } else {
            result += c;
            space = false;
        }
    }
    
    // Trim trailing space if present
    if (!result.empty() && result.back() == ' ')
        result.pop_back();
        
    return result;
}

// Function to extract job listing details from a node
json scrape_details(GumboNode* n, const SiteConfig& cfg) {
    json j;
    j["site"] = cfg.name;
    j["scraped_at"] = now_iso();
    
    struct Field {
        std::string tag, cls, key;
    } fields[] = {
        {cfg.title_tag, cfg.title_class, "title"},
        {cfg.company_tag, cfg.company_class, "company"},
        {cfg.location_tag, cfg.location_class, "location"},
        {cfg.salary_tag, cfg.salary_class, "salary"},
        {cfg.description_tag, cfg.description_class, "description"},
        {cfg.date_tag, cfg.date_class, "posted_date"},
        {cfg.skills_tag, cfg.skills_class, "skills"}
    };
    
    for (auto& field : fields) {
        if (field.tag.empty()) continue;
        std::vector<GumboNode*> nodes;
        find_nodes(n, field.tag, field.cls, nodes);
        
        if (!nodes.empty()) {
            j[field.key] = clean_text(extract_text(nodes[0]));
            
            // Handle special case for title with URL
            if (field.key == "title") {
                // Try to extract URL from title element if it's an anchor
                std::string href;
                if (field.tag == "a") {
                    href = extract_attr(nodes[0], "href");
                } else {
                    // Look for first anchor within the title element
                    std::vector<GumboNode*> anchors;
                    find_nodes(nodes[0], "a", "", anchors);
                    if (!anchors.empty()) href = extract_attr(anchors[0], "href");
                }
                
                if (!href.empty()) j["url"] = normalize_url(href, cfg.base_url);
            }
        }
    }
    
    // Generate a unique ID for the job
    std::string id_str = j.value("site", "") + ":" + j.value("title", "") + ":" + j.value("company", "");
    j["job_id"] = std::to_string(std::hash<std::string>{}(id_str));
    
    return j;
}

// Function to send job data to an API endpoint
bool send_to_api(const json& job_data, const APIConfig& api_cfg) {
    if (!api_cfg.enabled) return false;
    
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    std::string response;
    std::string payload = job_data.dump();
    
    curl_easy_setopt(curl, CURLOPT_URL, api_cfg.endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, payload.length());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!api_cfg.auth_token.empty()) {
        std::string auth_header = "Authorization: Bearer " + api_cfg.auth_token;
        headers = curl_slist_append(headers, auth_header.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    
    bool success = (res == CURLE_OK && (http_code >= 200 && http_code < 300));
    if (!success) {
        std::cerr << "API request failed: " << curl_easy_strerror(res) 
                 << ", HTTP code: " << http_code << std::endl;
        if (!response.empty()) std::cerr << "Response: " << response.substr(0, 200) << std::endl;
    }
    
    return success;
}

// Function to escape special characters for CSV
std::string escape_csv(const std::string& s) {
    std::string r = s;
    size_t p = 0;
    while ((p = r.find('"', p)) != std::string::npos) { 
        r.insert(p, "\""); 
        p += 2; 
    }
    return r;
}

// Function to save scraped jobs to JSON file
void save_to_json(const std::vector<json>& jobs, const std::string& filepath) {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open JSON file for writing: " << filepath << std::endl;
        return;
    }
    
    file << json(jobs).dump(4);
    file.close();
}

// Function to save scraped jobs to CSV file
void save_to_csv(const std::vector<json>& jobs, const std::string& filepath) {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open CSV file for writing: " << filepath << std::endl;
        return;
    }
    
    // Write CSV header
    file << "job_id,site,title,company,location,salary,description,posted_date,skills,url,scraped_at\n";
    
    // Write job data
    for (const auto& job : jobs) {
        file << '"' << job.value("job_id", "") << "\",";
        file << '"' << escape_csv(job.value("site", "")) << "\",";
        file << '"' << escape_csv(job.value("title", "")) << "\",";
        file << '"' << escape_csv(job.value("company", "")) << "\",";
        file << '"' << escape_csv(job.value("location", "")) << "\",";
        file << '"' << escape_csv(job.value("salary", "")) << "\",";
        file << '"' << escape_csv(job.value("description", "")) << "\",";
        file << '"' << escape_csv(job.value("posted_date", "")) << "\",";
        file << '"' << escape_csv(job.value("skills", "")) << "\",";
        file << '"' << escape_csv(job.value("url", "")) << "\",";
        file << '"' << escape_csv(job.value("scraped_at", "")) << "\"\n";
    }
    
    file.close();
}

// Main function
int main(int argc, char** argv) {
    // Initialize CURL globally
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    // Parse command line arguments (simplified)
    APIConfig api_cfg;
    OutputConfig output_cfg;
    bool use_db = false;
    std::string db_conn_string;
    
    // Check for command-line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "--api" && i + 2 < argc) {
            api_cfg.endpoint = argv[++i];
            api_cfg.auth_token = argv[++i];
            api_cfg.enabled = true;
        }
        else if (arg == "--output-dir" && i + 1 < argc) {
            output_cfg.output_dir = argv[++i];
        }
        else if (arg == "--no-json") {
            output_cfg.json_output = false;
        }
        else if (arg == "--no-csv") {
            output_cfg.csv_output = false;
        }
        else if (arg == "--interval" && i + 1 < argc) {
            output_cfg.scrape_interval = std::chrono::hours(std::stoi(argv[++i]));
        }
        else if (arg == "--db" && i + 1 < argc) {
            use_db = true;
            db_conn_string = argv[++i];
        }
        else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  --api ENDPOINT TOKEN    Send jobs to API endpoint with auth token\n"
                      << "  --output-dir DIR        Set output directory for files\n"
                      << "  --no-json               Disable JSON output\n"
                      << "  --no-csv                Disable CSV output\n"
                      << "  --interval HOURS        Set scraping interval in hours\n"
                      << "  --db CONNSTRING         Enable PostgreSQL database output\n"
                      << "  --help                  Show this help message\n";
            return 0;
        }
    }
    
    // Create output directory if it doesn't exist
    try {
        if (!fs::exists(output_cfg.output_dir)) {
            fs::create_directories(output_cfg.output_dir);
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Error creating output directory: " << e.what() << std::endl;
        return 1;
    }
    
    // Configure job sites to scrape
    std::vector<SiteConfig> sites;
    sites.reserve(7);
    
    // Indeed
    sites.push_back({
        "Indeed", 
        "https://www.indeed.com", 
        "https://www.indeed.com/jobs?q=Software+Developer&l=New+York",
        "div", "job_seen_beacon", 
        "h2", "jobTitle", 
        "span", "companyName", 
        "div", "companyLocation",
        "div", "salary-snippet-container", 
        "div", "job-snippet", 
        "span", "date", 
        "", "", 
        "start", 
        2
    });
    
    // Monster
    sites.push_back({
        "Monster", 
        "https://www.monster.com", 
        "https://www.monster.com/jobs/search/?q=Software-Developer&where=New-York",
        "section", "card-content", 
        "h2", "title", 
        "div", "company", 
        "div", "location",
        "", "", 
        "div", "summary", 
        "", "", 
        "", "", 
        "page", 
        2
    });
    
    // LinkedIn
    sites.push_back({
        "LinkedIn", 
        "https://www.linkedin.com", 
        "https://www.linkedin.com/jobs/search?keywords=Software%20Engineer&location=Worldwide",
        "li", "result-card", 
        "h3", "base-search-card__title", 
        "h4", "base-search-card__subtitle", 
        "span", "job-result-card__location",
        "", "", 
        "p", "job-snippet", 
        "time", "", 
        "", "", 
        "start", 
        2,
        3s  // LinkedIn is more aggressive against scrapers, wait longer
    });
    
    // Glassdoor
    sites.push_back({
        "Glassdoor", 
        "https://www.glassdoor.com", 
        "https://www.glassdoor.com/Job/software-engineer-jobs-SRCH_KO0,17.htm",
        "li", "react-job-listing", 
        "a", "jobLink", 
        "div", "jobHeader a", 
        "span", "subtle loc",
        "", "", 
        "div", "jobDescriptionContent", 
        "span", "minor", 
        "", "", 
        "", 
        2
    });
    
    // Dice
    sites.push_back({
        "Dice", 
        "https://www.dice.com", 
        "https://www.dice.com/jobs?q=Software+Developer&l=New+York",
        "div", "card-company-action", 
        "a", "card-title-link", 
        "span", "card-company", 
        "span", "job-location",
        "span", "salary", 
        "div", "complete-serp-result-div", 
        "time", "date", 
        "", "", 
        "", 
        2
    });
    
    // RemoteOK
    sites.push_back({
        "RemoteOK", 
        "https://remoteok.com", 
        "https://remoteok.com/remote-dev-jobs",
        "tr", "job", 
        "td", "company_and_position", 
        "td", "company", 
        "td", "region",
        "td", "salary", 
        "td", "description", 
        "time", "", 
        "", "", 
        "", 
        1
    });
    
    // WeWorkRemotely
    sites.push_back({
        "WeWorkRemotely", 
        "https://weworkremotely.com", 
        "https://weworkremotely.com/categories/remote-programming-jobs",
        "section", "jobs", 
        "li", "feature", 
        "span", "company", 
        "span", "region",
        "", "", 
        "div", "job-listing-left", 
        "time", "date", 
        "", "", 
        "", 
        1
    });

    sites.push_back({
        "RemoteCo", 
        "https://remote.co", 
        "https://remote.co/remote-jobs/developer/",
        "div", "job_listing", 
        "h3", "job_title", 
        "p", "company", 
        "div", "location",
        "", "", 
        "div", "summary", 
        "date", "date", 
        "div", "job_tags", 
        "page", 
        2
    });
    
    // FlexJobs
    sites.push_back({
        "FlexJobs", 
        "https://www.flexjobs.com", 
        "https://www.flexjobs.com/search?search=software+developer",
        "div", "job-listing", 
        "h5", "job-title", 
        "div", "job-company", 
        "div", "job-locations",
        "", "", 
        "div", "job-description",
        "span", "job-age", 
        "div", "job-category", 
        "page", 
        2
    });
    
    // Stack Overflow Jobs
    sites.push_back({
        "StackOverflow", 
        "https://stackoverflow.com", 
        "https://stackoverflow.com/jobs?q=software+developer",
        "div", "-job", 
        "h2", "mb4", 
        "h3", "fc-black-700", 
        "div", "fc-black-700",
        "span", "salary", 
        "div", "fs-body1", 
        "span", "ps-relative", 
        "div", "d-grid", 
        "page", 
        2,
        4s  // Stack Overflow might be sensitive to scraping
    });
    
    // JustRemote
    sites.push_back({
        "JustRemote", 
        "https://justremote.co", 
        "https://justremote.co/remote-developer-jobs",
        "div", "job-list-item", 
        "h3", "job-title", 
        "p", "company-name", 
        "div", "job-location",
        "", "", 
        "div", "job-description", 
        "div", "job-date", 
        "", "", 
        "page", 
        1
    });
    
    // Working Nomads
    sites.push_back({
        "WorkingNomads", 
        "https://www.workingnomads.com", 
        "https://www.workingnomads.com/jobs?category=development",
        "a", "job-container", 
        "h4", "job-title", 
        "h5", "company", 
        "p", "location-container",
        "", "", 
        "div", "exceprt", 
        "a", "job-date", 
        "p", "tags", 
        "page", 
        2
    });
    
    // AngelList
    sites.push_back({
        "AngelList", 
        "https://angel.co", 
        "https://angel.co/jobs",
        "div", "job-listing", 
        "h3", "title", 
        "div", "company-name", 
        "div", "location",
        "div", "salary", 
        "div", "description", 
        "div", "posted-date", 
        "div", "skills", 
        "page", 
        1,
        5s  // AngelList is likely to have stricter anti-scraping measures
    });
    
    // Optional: Connect to PostgreSQL if requested
    std::optional<DBConfig> db_config;
    if (use_db) {
        db_config = DBConfig(db_conn_string);
        // Note: For actual database operations, you would need to include and use pqxx
    }
    
    std::cout << "Job scraper started. Press Ctrl+C to exit.\n";
    
    // Main scraping loop
    while (true) {
        std::vector<json> all_jobs;
        std::time_t scrape_time = std::time(nullptr);
        
        std::cout << "Starting scrape cycle at: " << std::put_time(std::localtime(&scrape_time), "%Y-%m-%d %H:%M:%S") << std::endl;
        
        // Process each site
        for (const auto& site : sites) {
            try {
                std::cout << "Scraping " << site.name << "..." << std::endl;
                
                for (int page = 1; page <= site.max_pages; ++page) {
                    std::string url = site.search_url;
                    if (!site.pagination_param.empty()) {
                        url += (url.find('?') != std::string::npos ? "&" : "?") + 
                              site.pagination_param + "=" + std::to_string(page);
                    }
                    
                    std::cout << "  Fetching page " << page << " from " << site.name << std::endl;
                    
                    // Fetch and parse the page
                    std::string html;
                    try {
                        html = fetch_page(url);
                    } catch (const ScraperException& e) {
                        std::cerr << "Error fetching " << site.name << " page " << page << ": " << e.what() << std::endl;
                        continue;
                    }
                    
                    GumboOutput* parsed_output = gumbo_parse(html.c_str());
                    std::vector<GumboNode*> job_nodes;
                    find_nodes(parsed_output->root, site.container_tag, site.container_class, job_nodes);
                    
                    std::cout << "  Found " << job_nodes.size() << " job listings on page " << page << std::endl;
                    
                    // Process each job listing
                    for (auto* node : job_nodes) {
                        json job = scrape_details(node, site);
                        if (!job.value("title", "").empty()) {
                            all_jobs.push_back(job);
                            
                            // Send to API if configured
                            if (api_cfg.enabled) {
                                send_to_api(job, api_cfg);
                            }
                        }
                    }
                    
                    // Clean up Gumbo parser resources
                    gumbo_destroy_output(&kGumboDefaultOptions, parsed_output);
                    
                    // Respect site's scraping delay
                    std::this_thread::sleep_for(site.delay);
                }
            } catch (const std::exception& e) {
                std::cerr << "Error processing site " << site.name << ": " << e.what() << std::endl;
            }
        }
        
        // Generate timestamp for filenames
        std::stringstream ts;
        ts << std::put_time(std::localtime(&scrape_time), "%Y%m%d_%H%M%S");
        std::string timestamp = ts.str();
        
        // Save to JSON if enabled
        if (output_cfg.json_output) {
            std::string json_path = output_cfg.output_dir + "/jobs_" + timestamp + ".json";
            save_to_json(all_jobs, json_path);
            std::cout << "Saved " << all_jobs.size() << " jobs to " << json_path << std::endl;
        }
        
        // Save to CSV if enabled
        if (output_cfg.csv_output) {
            std::string csv_path = output_cfg.output_dir + "/jobs_" + timestamp + ".csv";
            save_to_csv(all_jobs, csv_path);
            std::cout << "Saved " << all_jobs.size() << " jobs to " << csv_path << std::endl;
        }
        
        // Wait for next scrape cycle
        std::cout << "Scrape cycle completed. Waiting for " << output_cfg.scrape_interval.count() 
                 << " hours before next cycle." << std::endl;
        std::this_thread::sleep_for(output_cfg.scrape_interval);
    }
    
    // Clean up global CURL resources
    curl_global_cleanup();
    
    return 0;
}



//g++ -std=c++17 -o scraper scraper.cpp \
    -I/usr/local/include \
    -I/usr/local/opt/libpq/include \
    -L/usr/local/lib \
    -L/usr/local/opt/libpq/lib \
    -lcurl -lgumbo -lpqxx -lpq -pthread

