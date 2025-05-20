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
#include <regex>
#include <set>
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
    std::string search_url_template; // Template with {job_title} and {location} placeholders
    std::string container_tag, container_class;
    std::string title_tag, title_class;
    std::string company_tag, company_class;
    std::string location_tag, location_class;
    std::string description_tag, description_class;
    std::string url_tag, url_class;  // For job URL extraction
    std::string date_tag, date_class;
    std::string skills_tag, skills_class;
    std::string pagination_param;
    int max_pages;
    std::chrono::seconds delay{2}; // Configurable delay between requests
    bool requires_js{false};       // Indicates if the site requires JavaScript for content
};

// Output configuration
struct OutputConfig {
    bool json_output{true};
    bool sqlite_output{false};
    std::string sqlite_db_path;
    std::string output_dir{"./output"};
    std::chrono::hours scrape_interval{1};
    int max_jobs{100}; // Maximum number of jobs to scrape per run
};

// Search configuration
struct SearchConfig {
    std::string job_title{"Software Developer"};
    std::string location{"Remote"};
    std::vector<std::string> keywords;
    bool extract_skills{true};
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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // For https sites
    
    // Add request headers to look more like a browser
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8");
    headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.5");
    headers = curl_slist_append(headers, "Cache-Control: no-cache");
    headers = curl_slist_append(headers, "Pragma: no-cache");
    headers = curl_slist_append(headers, "DNT: 1");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    
    CURLcode res = CURLE_OK;
    for (int i = 0; i < retries; i++) {
        res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            
            if (http_code >= 200 && http_code < 300) {
                break;
            } else {
                std::cerr << "HTTP error: " << http_code << " for URL: " << url << std::endl;
                // If it's a 429 (too many requests), wait longer
                if (http_code == 429) {
                    std::this_thread::sleep_for(std::chrono::seconds(10 * (i + 1)));
                }
            }
        } else {
            std::cerr << "CURL attempt " << (i+1) << " failed: " << curl_easy_strerror(res) << std::endl;
        }
        
        // Exponential backoff
        std::this_thread::sleep_for(std::chrono::seconds(2 * (i + 1)));
    }
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) 
        throw ScraperException(std::string("CURL error after retries: ") + curl_easy_strerror(res));
    
    return buffer;
}

// Function that was forward-declared earlier but needed implementation
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
    
    // Handle relative URL without leading slash
    std::string base = base_url;
    size_t last_slash = base.find_last_of('/');
    if (last_slash != std::string::npos && last_slash > 8) { // 8 is to account for http:// or https://
        base = base.substr(0, last_slash + 1);
    } else if (base.back() != '/') {
        base += '/';
    }
    
    return base + url;
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

// Function to extract URL from a node (checks for href attributes or nested a tags)
std::string extract_url(GumboNode* node, const std::string& base_url) {
    if (!node) return "";
    
    // Check if this node is an anchor with href
    std::string href = extract_attr(node, "href");
    if (!href.empty()) return normalize_url(href, base_url);
    
    // Search for first anchor tag within this node
    std::vector<GumboNode*> anchors;
    find_nodes(node, "a", "", anchors);
    if (!anchors.empty()) {
        href = extract_attr(anchors[0], "href");
        if (!href.empty()) return normalize_url(href, base_url);
    }
    
    return "";
}

// Function to get the current timestamp in ISO format
std::string now_iso() {
    auto t = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(t);
    std::ostringstream o;
    o << std::put_time(std::localtime(&tt), "%Y-%m-%d %H:%M:%S");
    return o.str();
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

// Function to extract skills from job description using common skill keywords
std::vector<std::string> extract_skills(const std::string& description) {
    // List of common skill keywords to look for
    // This is a simplified approach - in a real implementation, you might want to use NLP
    static const std::vector<std::string> common_skills = {
        // Programming Languages
        "Python", "Java", "JavaScript", "C++", "C#", "Ruby", "PHP", "Go", "Swift", "Kotlin", "Rust",
        // Web Technologies
        "HTML", "CSS", "React", "Angular", "Vue", "Node.js", "Express", "Django", "Flask", "Spring Boot",
        "REST API", "GraphQL", "JSON", "XML", "Bootstrap", "jQuery", "TypeScript",
        // Data Science & AI
        "Machine Learning", "Deep Learning", "NLP", "Computer Vision", "Data Analysis", "Statistics",
        "TensorFlow", "PyTorch", "Keras", "scikit-learn", "pandas", "NumPy", "R", "SQL", "NoSQL",
        "Data Mining", "Big Data", "Data Visualization", "Tableau", "Power BI", "AI",
        // DevOps & Cloud
        "AWS", "Azure", "Google Cloud", "Docker", "Kubernetes", "CI/CD", "Jenkins", "GitHub Actions",
        "Terraform", "Ansible", "Chef", "Puppet", "Linux", "Unix", "Windows Server", "DevOps",
        // Databases
        "MySQL", "PostgreSQL", "MongoDB", "Oracle", "SQL Server", "Redis", "Elasticsearch", "Cassandra",
        "DynamoDB", "Firebase", "Neo4j", "GraphDB", "Database Design", "Query Optimization",
        // Mobile Development
        "Android", "iOS", "React Native", "Flutter", "Xamarin", "Mobile Development", "App Development",
        // Software Engineering Practices
        "Agile", "Scrum", "Kanban", "Unit Testing", "TDD", "BDD", "Code Review", "Version Control",
        "Git", "SVN", "Mercurial", "Design Patterns", "OOP", "Functional Programming", "Microservices",
        "API Design", "System Design", "Software Architecture", "SOLID Principles", "Clean Code",
        // Project Management
        "Project Management", "Jira", "Confluence", "Trello", "Asana", "MS Project", "Stakeholder Management",
        // Soft Skills (these are harder to detect but still important)
        "Communication", "Team Leadership", "Problem Solving", "Critical Thinking", "Collaboration",
        // Other Technical Skills
        "UI/UX Design", "Figma", "Sketch", "Adobe XD", "Photoshop", "Illustrator", "SEO",
        "Analytics", "Digital Marketing", "Content Marketing", "SaaS", "ERP", "CRM",
        "Networking", "Cybersecurity", "Blockchain", "Cryptocurrency", "VR/AR", "Game Development",
        "Embedded Systems", "IoT", "Robotics", "Full Stack", "Front End", "Back End"
    };
    
    std::set<std::string> found_skills;
    std::string desc_upper = description;
    std::transform(desc_upper.begin(), desc_upper.end(), desc_upper.begin(), ::toupper);
    
    for (const auto& skill : common_skills) {
        std::string skill_upper = skill;
        std::transform(skill_upper.begin(), skill_upper.end(), skill_upper.begin(), ::toupper);
        
        if (desc_upper.find(skill_upper) != std::string::npos) {
            found_skills.insert(skill);
        }
    }
    
    // Convert set to vector for return
    return std::vector<std::string>(found_skills.begin(), found_skills.end());
}

// URL encode function for creating search URLs
std::string url_encode(const std::string& value) {
    CURL* curl = curl_easy_init();
    if (!curl) throw ScraperException("Failed to initialize CURL for URL encoding");
    
    char* output = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.length()));
    if (!output) {
        curl_easy_cleanup(curl);
        throw ScraperException("Failed to URL encode string: " + value);
    }
    
    std::string result(output);
    curl_free(output);
    curl_easy_cleanup(curl);
    
    return result;
}

// Function to replace placeholders in URL templates
std::string format_url(const std::string& url_template, 
                       const std::string& job_title, 
                       const std::string& location) {
    std::string url = url_template;
    
    // Replace {job_title} with URL-encoded job title
    size_t pos = url.find("{job_title}");
    if (pos != std::string::npos) {
        url.replace(pos, 11, url_encode(job_title));
    }
    
    // Replace {location} with URL-encoded location
    pos = url.find("{location}");
    if (pos != std::string::npos) {
        url.replace(pos, 10, url_encode(location));
    }
    
    return url;
}

// Function to extract job listing details from a node
json scrape_details(GumboNode* n, const SiteConfig& cfg, const SearchConfig& search_cfg) {
    json j;
    j["source"] = cfg.name;
    j["scraped_at"] = now_iso();
    
    // Extract title
    if (!cfg.title_tag.empty()) {
        std::vector<GumboNode*> nodes;
        find_nodes(n, cfg.title_tag, cfg.title_class, nodes);
        if (!nodes.empty()) {
            j["title"] = clean_text(extract_text(nodes[0]));
        }
    }
    
    // Extract location
    if (!cfg.location_tag.empty()) {
        std::vector<GumboNode*> nodes;
        find_nodes(n, cfg.location_tag, cfg.location_class, nodes);
        if (!nodes.empty()) {
            j["location"] = clean_text(extract_text(nodes[0]));
        } else {
            // Use search location if we couldn't find it in the listing
            j["location"] = search_cfg.location;
        }
    } else {
        // Default to search location
        j["location"] = search_cfg.location;
    }
    
    // Extract company (optional for your format)
    if (!cfg.company_tag.empty()) {
        std::vector<GumboNode*> nodes;
        find_nodes(n, cfg.company_tag, cfg.company_class, nodes);
        if (!nodes.empty()) {
            j["company"] = clean_text(extract_text(nodes[0]));
        }
    }
    
    // Extract description
    if (!cfg.description_tag.empty()) {
        std::vector<GumboNode*> nodes;
        find_nodes(n, cfg.description_tag, cfg.description_class, nodes);
        if (!nodes.empty()) {
            j["description"] = clean_text(extract_text(nodes[0]));
        }
    }
    
    // Extract URL
    std::string job_url;
    if (!cfg.url_tag.empty()) {
        std::vector<GumboNode*> nodes;
        find_nodes(n, cfg.url_tag, cfg.url_class, nodes);
        if (!nodes.empty()) {
            job_url = extract_url(nodes[0], cfg.base_url);
        }
    } else {
        // Try to extract from title element or container
        job_url = extract_url(n, cfg.base_url);
    }
    
    if (!job_url.empty()) {
        j["source"] = job_url;
    }
    
    // Extract skills - either from a dedicated field or from description
    std::vector<std::string> skills;
    if (!cfg.skills_tag.empty()) {
        std::vector<GumboNode*> nodes;
        find_nodes(n, cfg.skills_tag, cfg.skills_class, nodes);
        if (!nodes.empty()) {
            std::string skills_text = clean_text(extract_text(nodes[0]));
            
            // Basic tokenization assuming comma or bullet separation
            std::istringstream iss(skills_text);
            std::string skill;
            while (std::getline(iss, skill, ',')) {
                if (!skill.empty()) {
                    // Clean up whitespace and add to skills array
                    skill = std::regex_replace(skill, std::regex("^\\s+|\\s+$"), "");
                    if (!skill.empty()) {
                        skills.push_back(skill);
                    }
                }
            }
        }
    }
    
    // If no skills found in dedicated field and extract_skills is enabled, extract from description
    if (skills.empty() && search_cfg.extract_skills && j.contains("description")) {
        skills = extract_skills(j["description"]);
    }
    
    // If we have skills, add them to the JSON
    if (!skills.empty()) {
        j["skills"] = skills;
    } else {
        // Empty array if no skills found
        j["skills"] = json::array();
    }
    
    // Apply keyword filtering if specified
    if (!search_cfg.keywords.empty()) {
        bool match = false;
        std::string description = j.value("description", "");
        std::string title = j.value("title", "");
        std::transform(description.begin(), description.end(), description.begin(), ::tolower);
        std::transform(title.begin(), title.end(), title.begin(), ::tolower);
        
        for (const auto& keyword : search_cfg.keywords) {
            std::string keyword_lower = keyword;
            std::transform(keyword_lower.begin(), keyword_lower.end(), keyword_lower.begin(), ::tolower);
            
            if (description.find(keyword_lower) != std::string::npos || 
                title.find(keyword_lower) != std::string::npos) {
                match = true;
                break;
            }
        }
        
        if (!match) {
            // If no match with keywords, return an empty JSON
            return json();
        }
    }
    
    return j;
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

#ifdef ENABLE_SQLITE
// SQLite implementation (when ENABLE_SQLITE is defined)
#include <sqlite3.h>

bool init_sqlite_db(const std::string& db_path) {
    sqlite3* db;
    char* err_msg = nullptr;
    
    // Open database connection
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        std::cerr << "Cannot open database: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        return false;
    }
    
    // SQL statement to create jobs table if it doesn't exist
    const char* sql = 
        "CREATE TABLE IF NOT EXISTS jobs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "title TEXT NOT NULL,"
        "company TEXT,"
        "location TEXT,"
        "description TEXT,"
        "source TEXT,"
        "source_url TEXT,"
        "scraped_at TEXT,"
        "skills TEXT"
        ");";
    
    // Execute SQL
    rc = sqlite3_exec(db, sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "SQL error: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return false;
    }
    
    // Close database
    sqlite3_close(db);
    return true;
}

bool save_to_sqlite(const std::vector<json>& jobs, const std::string& db_path) {
    sqlite3* db;
    char* err_msg = nullptr;
    sqlite3_stmt* stmt;
    
    // Open database connection
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        std::cerr << "Cannot open database: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        return false;
    }
    
    // Begin transaction for better performance
    rc = sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to begin transaction: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return false;
    }
    
    // Prepare SQL statement
    const char* sql = 
        "INSERT INTO jobs (title, company, location, description, source, source_url, scraped_at, skills) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
    
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        return false;
    }
    
    // Insert all jobs
    for (const auto& job : jobs) {
        // Bind parameters
        sqlite3_bind_text(stmt, 1, job.value("title", "").c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, job.value("company", "").c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, job.value("location", "").c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, job.value("description", "").c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, job.value("source", "").c_str(), -1, SQLITE_STATIC);
        
        // For source_url, use URL if available, otherwise use an empty string
        std::string url = job.value("url", "");
        sqlite3_bind_text(stmt, 6, url.c_str(), -1, SQLITE_STATIC);
        
        sqlite3_bind_text(stmt, 7, job.value("scraped_at", "").c_str(), -1, SQLITE_STATIC);
        
        // Convert skills array to comma-separated string
        std::string skills_str;
        if (job.contains("skills") && job["skills"].is_array()) {
            for (const auto& skill : job["skills"]) {
                if (!skills_str.empty()) skills_str += ", ";
                skills_str += skill.get<std::string>();
            }
        }
        sqlite3_bind_text(stmt, 8, skills_str.c_str(), -1, SQLITE_STATIC);
        
        // Execute statement
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            std::cerr << "Failed to insert job: " << sqlite3_errmsg(db) << std::endl;
        }
        
        // Reset statement for next insertion
        sqlite3_reset(stmt);
    }
    
    // Finalize statement
    sqlite3_finalize(stmt);
    
    // Commit transaction
    rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to commit transaction: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return false;
    }
    
    // Close database
    sqlite3_close(db);
    return true;
}
#endif // ENABLE_SQLITE

// Function to configure job search sites
std::vector<SiteConfig> initialize_site_configs() {
    std::vector<SiteConfig> sites;
    
    // LinkedIn - Updated with search URL template
    sites.push_back({
        "LinkedIn", 
        "https://www.linkedin.com", 
        "https://www.linkedin.com/jobs/search?keywords={job_title}&location={location}",
        "div", "base-card relative", 
        "h3", "base-search-card__title", 
        "h4", "base-search-card__subtitle", 
        "span", "job-search-card__location",
        "p", "base-search-card__metadata", 
        "a", "base-card__full-link", 
        "time", "", 
        "", "", 
        "start", 
        2,
        3s
    });
    
    // Indeed - Updated with search URL template
    sites.push_back({
        "Indeed", 
        "https://www.indeed.com", 
        "https://www.indeed.com/jobs?q={job_title}&l={location}",
        "div", "job_seen_beacon", 
        "h2", "jobTitle", 
        "span", "companyName", 
        "div", "companyLocation",
        "div", "job-snippet", 
        "a", "jcs-JobTitle", 
        "span", "date", 
        "", "", 
        "start", 
        3,
        2s
    });
    
    // Glassdoor - Updated with search URL template
    sites.push_back({
        "Glassdoor", 
        "https://www.glassdoor.com", 
        "https://www.glassdoor.com/Job/jobs.htm?sc.keyword={job_title}&locT=C&locId=2950115&locKeyword={location}",
        "li", "react-job-listing", 
        "a", "jobLink", 
        "div", "job-search-results__company-name", 
        "span", "subtle loc",
        "div", "JobDescriptionContainer", 
        "a", "jobLink", 
        "div", "listing-age", 
        "", "", 
        "page", 
        2
    });
    
    // RemoteOK - Updated with search URL template
    sites.push_back({
        "RemoteOK", 
        "https://remoteok.com", 
        "https://remoteok.com/remote-{job_title}-jobs",
        "tr", "job", 
        "h2", "preventLink", 
        "h3", "companyLink", 
        "div", "location", 
        "div", "description", 
        "a", "url", 
        "time", "date", 
        "div", "tags", 
        "", 
        2
    });
    
    // WeWorkRemotely - Updated with search URL template
    sites.push_back({
        "WeWorkRemotely", 
        "https://weworkremotely.com", 
        "https://weworkremotely.com/remote-jobs/search?term={job_title}",
        "li", "feature", 
        "span", "title", 
        "span", "company", 
        "span", "region",
        "div", "job-listing-left", 
        "a", "", 
        "span", "date", 
        "", "", 
        "", 
        1
    });
    
    // Monster - Updated with search URL template
    sites.push_back({
        "Monster", 
        "https://www.monster.com", 
        "https://www.monster.com/jobs/search?q={job_title}&where={location}",
        "div", "job-cardstyle__JobCardStyles", 
        "h3", "title", 
        "span", "company", 
        "span", "location",
        "div", "descriptionstyle__DescriptionStyles", 
        "a", "job-cardstyle__JobCardComponent", 
        "time", "postedDate", 
        "", "", 
        "page", 
        2
    });
    
    // SimplyHired - New site added
    sites.push_back({
        "SimplyHired", 
        "https://www.simplyhired.com", 
        "https://www.simplyhired.com/search?q={job_title}&l={location}",
        "div", "SerpJob-jobCard", 
        "h3", "jobposting-title", 
        "span", "jobposting-company", 
        "span", "jobposting-location",
        "p", "jobposting-snippet", 
        "a", "card-link", 
        "span", "SerpJob-age", 
        "", "", 
        "pn", 
        2
    });
    
    // ZipRecruiter - New site added
    sites.push_back({
        "ZipRecruiter", 
        "https://www.ziprecruiter.com", 
        "https://www.ziprecruiter.com/jobs/search?q={job_title}&l={location}",
        "div", "job_content", 
        "h2", "job_title", 
        "a", "company_name", 
        "div", "location",
        "div", "job_description", 
        "a", "job_link", 
        "div", "job_posted", 
        "", "", 
        "page", 
        2
    });
    
    return sites;
}

// Print help message function
void print_help(char* program_name) {
    std::cout << "Job Scraper - Scrapes job listings from popular job sites\n\n"
              << "Usage: " << program_name << " [options]\n\n"
              << "Options:\n"
              << "  --job-title TITLE     Job title to search for (default: Software Developer)\n"
              << "  --location LOCATION   Location to search in (default: Remote)\n"
              << "  --output-dir DIR      Set output directory for files (default: ./output)\n"
              << "  --sqlite PATH         Enable SQLite output and set database path\n"
              << "  --interval HOURS      Set scraping interval in hours (default: 1)\n"
              << "  --max-jobs N          Maximum number of jobs to scrape (default: 100)\n"
              << "  --keyword WORD        Add keyword filter (can be used multiple times)\n"
              << "  --no-skills           Disable automatic skill extraction\n"
              << "  --help                Show this help message\n";
}

// Main function
int main(int argc, char** argv) {
    // Initialize CURL globally
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    // Default configurations
    SearchConfig search_cfg;
    OutputConfig output_cfg;
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--job-title" && i + 1 < argc) {
            search_cfg.job_title = argv[++i];
        }
        else if (arg == "--location" && i + 1 < argc) {
            search_cfg.location = argv[++i];
        }
        else if (arg == "--output-dir" && i + 1 < argc) {
            output_cfg.output_dir = argv[++i];
        }
        else if (arg == "--sqlite" && i + 1 < argc) {
            output_cfg.sqlite_output = true;
            output_cfg.sqlite_db_path = argv[++i];
        }
        else if (arg == "--interval" && i + 1 < argc) {
            output_cfg.scrape_interval = std::chrono::hours(std::stoi(argv[++i]));
        }
        else if (arg == "--max-jobs" && i + 1 < argc) {
            output_cfg.max_jobs = std::stoi(argv[++i]);
        }
        else if (arg == "--keyword" && i + 1 < argc) {
            search_cfg.keywords.push_back(argv[++i]);
        }
        else if (arg == "--no-skills") {
            search_cfg.extract_skills = false;
        }
        else if (arg == "--help") {
            print_help(argv[0]);
            return 0;
        }
        else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_help(argv[0]);
            return 1;
        }
    }
    
    // Create output directory if it doesn't exist
    try {
        if (!fs::exists(output_cfg.output_dir)) {fs::create_directories(output_cfg.output_dir);
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Error creating output directory: " << e.what() << std::endl;
        return 1;
    }
    
    // Get site configurations
    auto sites = initialize_site_configs();
    
    // Main scraping loop
    while (true) {
        std::cout << "=== Starting job scraping at " << now_iso() << " ===" << std::endl;
        std::cout << "Searching for: " << search_cfg.job_title << " in " << search_cfg.location << std::endl;
        
        // Collection to store all jobs
        std::vector<json> all_jobs;
        
        // Process each job site
        for (const auto& site : sites) {
            std::cout << "Scraping from: " << site.name << std::endl;
            
            try {
                // Format search URL with job title and location
                std::string base_search_url = format_url(site.search_url_template, 
                                                        search_cfg.job_title, 
                                                        search_cfg.location);
                
                // Process multiple pages up to max_pages
                for (int page = 1; page <= site.max_pages; ++page) {
                    // Construct pagination URL
                    std::string page_url = base_search_url;
                    if (!site.pagination_param.empty()) {
                        char separator = (page_url.find('?') != std::string::npos) ? '&' : '?';
                        page_url += separator + site.pagination_param + "=" + std::to_string(page);
                    }
                    
                    std::cout << "  Fetching page " << page << ": " << page_url << std::endl;
                    
                    // Fetch page HTML content
                    std::string html;
                    try {
                        html = fetch_page(page_url);
                    } catch (const ScraperException& e) {
                        std::cerr << "  Error fetching page: " << e.what() << std::endl;
                        break;
                    }
                    
                    // Parse HTML with Gumbo
                    GumboOutput* output = gumbo_parse(html.c_str());
                    if (!output) {
                        std::cerr << "  Failed to parse HTML for " << site.name << std::endl;
                        continue;
                    }
                    
                    // Find job listing containers
                    std::vector<GumboNode*> containers;
                    find_nodes(output->root, site.container_tag, site.container_class, containers);
                    
                    std::cout << "  Found " << containers.size() << " job listings" << std::endl;
                    
                    // Extract job details from each container
                    for (auto* container : containers) {
                        json job = scrape_details(container, site, search_cfg);
                        
                        // Only add valid jobs
                        if (!job.empty()) {
                            all_jobs.push_back(job);
                            
                            // Print basic info about the job
                            std::cout << "  Scraped: " 
                                    << job.value("title", "Unknown Title") << " at " 
                                    << job.value("company", "Unknown Company") << " in "
                                    << job.value("location", "Unknown Location") << std::endl;
                            
                            // Break if we've reached the maximum number of jobs
                            if (all_jobs.size() >= static_cast<size_t>(output_cfg.max_jobs)) {
                                std::cout << "  Reached maximum job limit (" << output_cfg.max_jobs << ")" << std::endl;
                                break;
                            }
                        }
                    }
                    
                    // Clean up Gumbo parser
                    gumbo_destroy_output(&kGumboDefaultOptions, output);
                    
                    // Break if we've reached the maximum number of jobs
                    if (all_jobs.size() >= static_cast<size_t>(output_cfg.max_jobs)) {
                        break;
                    }
                    
                    // Respect the site's delay between requests to avoid being blocked
                    std::this_thread::sleep_for(site.delay);
                }
            } catch (const std::exception& e) {
                std::cerr << "Error scraping " << site.name << ": " << e.what() << std::endl;
            }
        }
        
        // Generate timestamped filename for output
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << output_cfg.output_dir << "/jobs_" 
           << std::put_time(std::localtime(&now_time_t), "%Y%m%d_%H%M%S") << ".json";
        std::string output_path = ss.str();
        
        // Save to JSON file
        if (output_cfg.json_output && !all_jobs.empty()) {
            try {
                save_to_json(all_jobs, output_path);
                std::cout << "Saved " << all_jobs.size() << " jobs to " << output_path << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Error saving to JSON: " << e.what() << std::endl;
            }
        }
        
        // Save to SQLite if enabled
        if (output_cfg.sqlite_output && !all_jobs.empty()) {
#ifdef ENABLE_SQLITE
            try {
                if (init_sqlite_db(output_cfg.sqlite_db_path)) {
                    if (save_to_sqlite(all_jobs, output_cfg.sqlite_db_path)) {
                        std::cout << "Saved " << all_jobs.size() << " jobs to SQLite database" << std::endl;
                    } else {
                        std::cerr << "Failed to save jobs to SQLite" << std::endl;
                    }
                } else {
                    std::cerr << "Failed to initialize SQLite database" << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "Error with SQLite: " << e.what() << std::endl;
            }
#else
            std::cerr << "SQLite support not enabled. Recompile with ENABLE_SQLITE defined." << std::endl;
#endif
        }
        
        // If this is a one-time run (interval is zero), break the loop
        if (output_cfg.scrape_interval.count() <= 0) {
            break;
        }
        
        std::cout << "Sleeping for " << output_cfg.scrape_interval.count() 
                  << " hours before next scrape..." << std::endl;
        
        // Sleep for the configured interval before next scrape
        std::this_thread::sleep_for(output_cfg.scrape_interval);
    }
    
    // Clean up CURL
    curl_global_cleanup();
    
    return 0;
}





// Helper function to deduplicate jobs based on title and company
std::vector<json> deduplicate_jobs(const std::vector<json>& jobs) {
    std::vector<json> unique_jobs;
    std::set<std::string> seen_fingerprints;
    
    for (const auto& job : jobs) {
        // Create a fingerprint based on title and company
        std::string title = job.value("title", "");
        std::string company = job.value("company", "");
        std::transform(title.begin(), title.end(), title.begin(), ::tolower);
        std::transform(company.begin(), company.end(), company.begin(), ::tolower);
        
        // Simple fingerprint using title and company
        std::string fingerprint = title + "|" + company;
        
        // Only add if we haven't seen this fingerprint before
        if (seen_fingerprints.find(fingerprint) == seen_fingerprints.end()) {
            seen_fingerprints.insert(fingerprint);
            unique_jobs.push_back(job);
        }
    }
    
    return unique_jobs;
}

// Function to export jobs to CSV format
void save_to_csv(const std::vector<json>& jobs, const std::string& filepath) {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open CSV file for writing: " << filepath << std::endl;
        return;
    }
    
    // Write CSV header
    file << "Title,Company,Location,Description,Source,Source URL,Scraped At,Skills\n";
    
    // Write job data
    for (const auto& job : jobs) {
        // Helper function to escape CSV fields
        auto escape_csv = [](const std::string& s) -> std::string {
            if (s.find_first_of(",\"\n") != std::string::npos) {
                std::string escaped = s;
                // Replace " with ""
                size_t pos = 0;
                while ((pos = escaped.find("\"", pos)) != std::string::npos) {
                    escaped.replace(pos, 1, "\"\"");
                    pos += 2;
                }
                return "\"" + escaped + "\"";
            }
            return s;
        };
        
        // Write each field, properly escaped
        file << escape_csv(job.value("title", "")) << ",";
        file << escape_csv(job.value("company", "")) << ",";
        file << escape_csv(job.value("location", "")) << ",";
        file << escape_csv(job.value("description", "")) << ",";
        file << escape_csv(job.value("source", "")) << ",";
        file << escape_csv(job.value("url", "")) << ",";
        file << escape_csv(job.value("scraped_at", "")) << ",";
        
        // Handle skills array
        std::string skills_str;
        if (job.contains("skills") && job["skills"].is_array()) {
            for (const auto& skill : job["skills"]) {
                if (!skills_str.empty()) skills_str += "; ";
                skills_str += skill.get<std::string>();
            }
        }
        file << escape_csv(skills_str) << "\n";
    }
    
    file.close();
}
