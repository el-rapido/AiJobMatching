#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <random>
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
struct SiteConfig
{
    std::string name;
    std::string base_url;
    std::string search_url_template; // Template with {job_title} and {location} placeholders
    std::string container_tag, container_class;
    std::string title_tag, title_class;
    std::string company_tag, company_class;
    std::string location_tag, location_class;
    std::string description_tag, description_class;
    std::string url_tag, url_class; // For job URL extraction
    std::string date_tag, date_class;
    std::string skills_tag, skills_class;
    std::string pagination_param;
    int max_pages;
    std::chrono::seconds delay{2}; // Configurable delay between requests
    bool requires_js{false};       // Indicates if the site requires JavaScript for content
};

// Output configuration
struct OutputConfig
{
    bool json_output{true};
    bool sqlite_output{false};
    std::string sqlite_db_path;
    std::string output_dir{"./output"};
    std::chrono::hours scrape_interval{1};
    int max_jobs{100}; // Maximum number of jobs to scrape per run
};

std::map<std::string, int> request_counts;
std::map<std::string, std::chrono::system_clock::time_point> last_request_time;

void rotate_job_sites(std::vector<SiteConfig> &sites)
{
    // Create a random number generator
    std::random_device rd;
    std::mt19937 g(rd()); // Use Mersenne Twister engine

    // Shuffle the sites vector
    std::shuffle(sites.begin(), sites.end(), g);
}

// Search configuration

struct SearchConfig
{
    std::string job_title{"Software Developer"};
    std::string location{"Remote"};
    std::vector<std::string> keywords;
    std::string target_site{""}; // Empty means scrape all sites
};

// Function declarations
json fetch_dice_job_details(const std::string &job_url, const SiteConfig &site_config, const SearchConfig &search_cfg);
json fetch_simplyhired_job_details(const std::string &job_url, const SiteConfig &site_config, const SearchConfig &search_cfg);

// Custom exception for error handling
class ScraperException : public std::runtime_error
{
public:
    ScraperException(const std::string &msg) : std::runtime_error(msg) {}
};

// CURL callback function for retrieving web content
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    ((std::string *)userp)->append((char *)contents, size * nmemb);
    return size * nmemb;
}

// Function to fetch a web page with error handling and retry logic
// Add this at the top of your file, near other globals
const std::vector<std::string> USER_AGENTS = {
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:122.0) Gecko/20100101 Firefox/122.0",
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/119.0.0.0 Safari/537.36",
    "Mozilla/5.0 (iPad; CPU OS 16_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.6 Mobile/15E148 Safari/604.1"};

// Enhanced fetch_page function with anti-detection measures
// Helper function to generate random strings for cookies and other values
std::string generate_random_string(size_t length)
{
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    std::string str;
    str.reserve(length);

    for (size_t i = 0; i < length; ++i)
    {
        str += alphanum[std::rand() % (sizeof(alphanum) - 1)];
    }

    return str;
}

// Rate limiting structure
struct RateLimitInfo
{
    std::chrono::seconds delay{5}; // Initial delay
    int consecutive_successes{0};
    int consecutive_failures{0};
    std::chrono::time_point<std::chrono::system_clock> last_request;
    bool backoff_mode{false};
};

// Global map to store rate limit info for each site
std::map<std::string, RateLimitInfo> rate_limits;

// Function to enforce rate limits before making requests
void enforce_rate_limits(const std::string &site_name)
{
    auto now = std::chrono::system_clock::now();

    if (rate_limits.find(site_name) == rate_limits.end())
    {
        // Initialize with default values
        rate_limits[site_name].delay = std::chrono::seconds(5);
        rate_limits[site_name].last_request = now - std::chrono::hours(1);
    }

    auto &info = rate_limits[site_name];

    // Calculate elapsed time since last request
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - info.last_request);

    // Determine appropriate delay based on site and current status
    std::chrono::seconds required_delay(5); // Default

    if (site_name == "LinkedIn")
    {
        required_delay = std::chrono::seconds(30);
    }
    else if (site_name == "Indeed")
    {
        required_delay = std::chrono::seconds(15);
    }
    else if (site_name == "Dice" || site_name == "SimplyHired")
    {
        required_delay = std::chrono::seconds(3);
    }

    // Apply backoff if needed
    if (info.backoff_mode)
    {
        required_delay *= 1.2;
        std::cout << "  Site " << site_name << " in backoff mode with " << required_delay.count() << "s delay" << std::endl;
    }

    // Add randomness
    required_delay += std::chrono::seconds(std::rand() % 5);

    // Wait if needed
    if (elapsed < required_delay)
    {
        auto wait_time = required_delay - elapsed;
        std::cout << "  Rate limiting: waiting " << wait_time.count() << " seconds for " << site_name << std::endl;
        std::this_thread::sleep_for(wait_time);
    }

    // Update last request time
    info.last_request = std::chrono::system_clock::now();
}

// Updated fetch_page function
std::string fetch_page(const std::string &url, int retries = 3, const std::string &site_name = "")
{
    // Apply rate limiting if site name is provided
    if (!site_name.empty())
    {
        enforce_rate_limits(site_name);
    }

    CURL *curl = curl_easy_init();
    if (!curl)
        throw ScraperException("Failed to initialize CURL");

    std::string buffer;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    // Enhanced browser impersonation with randomized details
    std::string browser_version = "120.0.0." + std::to_string(std::rand() % 100);
    std::string webkit_version = "537." + std::to_string(30 + (std::rand() % 9));

    // Create more realistic user agent
    std::vector<std::string> os_versions = {
        "Windows NT 10.0; Win64; x64",
        "Macintosh; Intel Mac OS X 10_15_7",
        "X11; Linux x86_64",
        "Windows NT 10.0; WOW64"};

    std::string os = os_versions[std::rand() % os_versions.size()];
    std::string user_agent = "Mozilla/5.0 (" + os + ") AppleWebKit/" + webkit_version +
                             " (KHTML, like Gecko) Chrome/" + browser_version + " Safari/" + webkit_version;

    // For tracking which user agent we're using
    size_t ua_index = 0;

    // Site-specific user agents for better success
    if (site_name == "LinkedIn")
    {
        std::vector<std::string> linkedin_agents = {
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36 Edg/120.0.0.0",
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.3 Safari/605.1.15",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:124.0) Gecko/20100101 Firefox/124.0"};
        ua_index = std::rand() % linkedin_agents.size();
        user_agent = linkedin_agents[ua_index];
    }
    else
    {
        // For other sites use the random user agent or from the global list if defined
        if (!USER_AGENTS.empty())
        {
            ua_index = std::rand() % USER_AGENTS.size();
            user_agent = USER_AGENTS[ua_index];
        }
    }

    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());

    // Add proxy support
    bool use_proxy = false; // Set to true when you have actual proxies
    if (use_proxy)
    {
        // List of proxies - replace with your actual proxy list
        static const std::vector<std::string> PROXIES = {
            "http://proxy1.example.com:8080",
            "http://proxy2.example.com:8080",
            "http://proxy3.example.com:8080"
            // Add your actual proxies here
        };

        if (!PROXIES.empty())
        {
            size_t proxy_index = std::rand() % PROXIES.size();
            std::string proxy = PROXIES[proxy_index];
            curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
            std::cout << "  Using proxy: " << proxy << std::endl;
        }
    }

    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    // Random 3-7 second delay
    int delay_ms = 3000 + (std::rand() % 4000);
    std::cout << "  Waiting for " << delay_ms / 1000.0 << " seconds before request..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

    // FIX: Properly handle compressed content
    // Some sites require this to be explicitly set to empty string rather than NULL
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    // Set referer based on site
    std::string referer;
    if (site_name == "Indeed")
    {
        referer = "https://www.indeed.com/";
    }
    else if (site_name == "LinkedIn")
    {
        referer = "https://www.linkedin.com/feed/";
    }
    else if (site_name == "ZipRecruiter")
    {
        referer = "https://www.ziprecruiter.com/";
    }
    else if (site_name == "SimplyHired")
    {
        referer = "https://www.simplyhired.com/";
    }
    else if (site_name == "Dice")
    {
        referer = "https://www.dice.com/";
    }
    else
    {
        // Extract domain for referer
        size_t pos = url.find("://");
        if (pos != std::string::npos)
        {
            size_t domain_end = url.find("/", pos + 3);
            if (domain_end != std::string::npos)
            {
                referer = url.substr(0, domain_end);
            }
            else
            {
                referer = url;
            }
        }
    }

    // Rotating cookie management
    std::string cookie_header;
    bool use_custom_cookies = true;

    if (use_custom_cookies)
    {
        if (site_name == "LinkedIn")
        {
            // LinkedIn specific cookies
            std::string li_at = generate_random_string(32);
            std::string jsession = generate_random_string(24);
            std::string lidc = generate_random_string(16);
            cookie_header = "li_at=" + li_at + "; JSESSIONID=ajax:" + jsession + "; lidc=b=" + lidc;
        }
        else if (site_name == "Indeed")
        {
            // Indeed specific cookies
            std::string ctk = generate_random_string(24);
            std::string csrf = generate_random_string(32);
            cookie_header = "CTK=" + ctk + "; INDEED_CSRF_TOKEN=" + csrf;
        }
        else if (site_name == "SimplyHired")
        {
            // SimplyHired specific cookies
            std::string csrf = generate_random_string(32);
            std::string shk = generate_random_string(16);
            std::string cf_id = generate_random_string(32);

            cookie_header = "csrf=" + csrf + "; shk=" + shk + "; _cfuvid=" + cf_id +
                            "; rq=%5B%22q%3DSoftware%2BDeveloper%26l%3DRemote%26ts%3D" +
                            std::to_string(time(NULL)) + "%22%5D";
        }
        else if (site_name == "Dice")
        {
            // Dice specific cookies
            std::string search_id = generate_random_string(16);
            std::string visitor_id = generate_random_string(24);
            cookie_header = "dice.search-id=" + search_id + "; dice.visitor-id=" + visitor_id;
        }
    }

    // Enable cookies (simulates browser cookie handling)
    static std::string cookie_file = "cookies.txt";
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookie_file.c_str());
    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, cookie_file.c_str());

    // Add request headers to look more like a browser
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8");
    headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.5");
    headers = curl_slist_append(headers, "Connection: keep-alive");
    headers = curl_slist_append(headers, "Upgrade-Insecure-Requests: 1");
    headers = curl_slist_append(headers, "Sec-Fetch-Dest: document");
    headers = curl_slist_append(headers, "Sec-Fetch-Mode: navigate");
    headers = curl_slist_append(headers, "Sec-Fetch-Site: none");
    headers = curl_slist_append(headers, "Sec-Fetch-User: ?1");
    headers = curl_slist_append(headers, "Cache-Control: max-age=0");

    // Browser fingerprint randomization
    headers = curl_slist_append(headers, ("Viewport-Width: " + std::to_string(1200 + (std::rand() % 400))).c_str());
    headers = curl_slist_append(headers, ("DPR: " + std::to_string(1 + (std::rand() % 2))).c_str());
    headers = curl_slist_append(headers, "Sec-CH-UA: \"Chromium\";v=\"110\"");
    headers = curl_slist_append(headers, "Sec-CH-UA-Mobile: ?0");
    headers = curl_slist_append(headers, "Sec-CH-UA-Platform: \"Windows\"");

    // Add custom cookie header if we generated one
    if (!cookie_header.empty())
    {
        headers = curl_slist_append(headers, ("Cookie: " + cookie_header).c_str());
    }

    if (!referer.empty())
    {
        std::string referer_header = "Referer: " + referer;
        headers = curl_slist_append(headers, referer_header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // Verbose output for debugging
    if (site_name == "Indeed" || site_name == "ZipRecruiter" || site_name == "SimplyHired" || site_name == "Dice")
    {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    }

    CURLcode res = CURLE_OK;
    for (int i = 0; i < retries; i++)
    {
        buffer.clear(); // Clear buffer for retry
        res = curl_easy_perform(curl);

        if (res == CURLE_OK)
        {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            if (http_code >= 200 && http_code < 300)
            {
                // Update rate limit info on success
                if (!site_name.empty())
                {
                    auto &info = rate_limits[site_name];
                    info.consecutive_successes++;
                    info.consecutive_failures = 0;

                    // Exit backoff mode if we've had several successes
                    if (info.backoff_mode && info.consecutive_successes > 3)
                    {
                        std::cout << "  Exiting backoff mode for " << site_name << std::endl;
                        info.backoff_mode = false;
                    }
                }
                break;
            }
            else
            {
                std::cerr << "HTTP error: " << http_code << " for URL: " << url << std::endl;

                // Update rate limit info on failure
                if (!site_name.empty())
                {
                    auto &info = rate_limits[site_name];
                    info.consecutive_failures++;
                    info.consecutive_successes = 0;

                    // Enter backoff mode on certain errors
                    if ((http_code == 429 || http_code == 403 || http_code == 999) && !info.backoff_mode)
                    {
                        std::cout << "  Entering backoff mode for " << site_name << std::endl;
                        info.backoff_mode = true;
                        info.delay *= 2;
                    }
                }

                // If it's a 429 (too many requests), wait longer
                if (http_code == 429)
                {
                    std::cerr << "Rate limited (429). Waiting longer..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(60 * (i + 1)));
                }
                else if (http_code == 403 || http_code == 999)
                {
                    std::cerr << "Forbidden (" << http_code << "). Site might be blocking scraping: " << site_name << std::endl;
                    // Save response for debugging
                    std::string debug_path = "debug_" + std::to_string(http_code) + "_" + site_name + "_" +
                                             std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".html";
                    std::ofstream debug_file(debug_path);
                    if (debug_file.is_open())
                    {
                        debug_file << buffer;
                        debug_file.close();
                        std::cerr << "Saved error response to: " << debug_path << std::endl;
                    }

                    // Try a different user agent
                    if (!USER_AGENTS.empty())
                    {
                        ua_index = (ua_index + 1) % USER_AGENTS.size();
                        curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENTS[ua_index].c_str());
                    }
                    else
                    {
                        // Use backup agents if USER_AGENTS isn't available
                        std::vector<std::string> backup_agents = {
                            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36 Edg/91.0.864.59",
                            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/14.1.1 Safari/605.1.15",
                            "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:89.0) Gecko/20100101 Firefox/89.0"};
                        std::string backup_agent = backup_agents[i % backup_agents.size()];
                        curl_easy_setopt(curl, CURLOPT_USERAGENT, backup_agent.c_str());
                    }

                    // Wait for a much longer time
                    std::this_thread::sleep_for(std::chrono::seconds(120 + (60 * i)));
                }
            }
        }
        else
        {
            std::cerr << "CURL attempt " << (i + 1) << " failed: " << curl_easy_strerror(res) << std::endl;

            // For compression issues, try with different encoding settings
            if (res == CURLE_BAD_CONTENT_ENCODING)
            {
                std::cerr << "Compression issue detected, trying different encoding settings..." << std::endl;
                curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
            }
        }

        // Exponential backoff with randomization
        int backoff_seconds = 3 * (i + 1) + (std::rand() % 5);
        std::this_thread::sleep_for(std::chrono::seconds(backoff_seconds));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        throw ScraperException(std::string("CURL error after retries: ") + curl_easy_strerror(res));

    return buffer;
}

// Special fetch_page function for LinkedIn
// Replace the current fetch_linkedin_page function with this simplified version
std::string fetch_linkedin_page(const std::string &url)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        throw ScraperException("Failed to initialize CURL");

    std::string buffer;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    // Add cookie support (minimal)
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, ""); // Enable cookies

    // Add only essential headers
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8");
    headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.5");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // Add a simple delay
    std::this_thread::sleep_for(std::chrono::seconds(2));

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK)
    {
        std::cerr << "LinkedIn CURL error: " << curl_easy_strerror(res) << std::endl;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        throw ScraperException(std::string("LinkedIn CURL error: ") + curl_easy_strerror(res));

    return buffer;
}
// Function that was forward-declared earlier but needed implementation
std::string normalize_url(const std::string &url, const std::string &base_url)
{
    if (url.empty())
        return "";
    if (url.rfind("http", 0) == 0)
        return url; // Already absolute

    // Handle various relative URL formats
    if (url[0] == '/')
    {
        // Extract domain from base_url
        size_t protocol_end = base_url.find("://");
        if (protocol_end == std::string::npos)
            return base_url + url;

        size_t domain_start = protocol_end + 3;
        size_t domain_end = base_url.find('/', domain_start);
        if (domain_end == std::string::npos)
            return base_url + url;

        return base_url.substr(0, domain_end) + url;
    }

    // Handle relative URL without leading slash
    std::string base = base_url;
    size_t last_slash = base.find_last_of('/');
    if (last_slash != std::string::npos && last_slash > 8)
    { // 8 is to account for http:// or https://
        base = base.substr(0, last_slash + 1);
    }
    else if (base.back() != '/')
    {
        base += '/';
    }

    return base + url;
}

// Function to recursively find nodes in HTML document
void find_nodes(GumboNode *node, const std::string &tag, const std::string &selector, std::vector<GumboNode *> &out)
{
    if (!node || node->type != GUMBO_NODE_ELEMENT)
        return;

    GumboElement &e = node->v.element;
    bool match_tag = tag.empty() || (e.tag != GUMBO_TAG_UNKNOWN &&
                                     std::string(gumbo_normalized_tagname(e.tag)) == tag);
    bool match_selector = selector.empty();

    if (!selector.empty())
    {
        // Check if selector is a data-testid attribute
        if (selector.find("data-testid=") != std::string::npos)
        {
            // Extract the expected testid value
            std::string testid_value = selector.substr(selector.find("=\"") + 2);
            testid_value = testid_value.substr(0, testid_value.find("\""));

            // Check if this node has that testid
            GumboAttribute *testid_attr = gumbo_get_attribute(&e.attributes, "data-testid");
            if (testid_attr && std::string(testid_attr->value) == testid_value)
            {
                match_selector = true;
            }
        }
        // Check if it's a class selector
        else if (selector.find("class=") != std::string::npos ||
                 selector.find("css-") != std::string::npos)
        {
            std::string class_value = selector;
            if (selector.find("class=\"") != std::string::npos)
            {
                class_value = selector.substr(selector.find("=\"") + 2);
                class_value = class_value.substr(0, class_value.find("\""));
            }

            GumboAttribute *class_attr = gumbo_get_attribute(&e.attributes, "class");
            if (class_attr && std::string(class_attr->value).find(class_value) != std::string::npos)
            {
                match_selector = true;
            }
        }
        // If it's neither, try a simple class match
        else
        {
            GumboAttribute *class_attr = gumbo_get_attribute(&e.attributes, "class");
            if (class_attr && std::string(class_attr->value).find(selector) != std::string::npos)
            {
                match_selector = true;
            }

            // If no class match, try as a direct attribute value
            if (!match_selector)
            {
                for (unsigned int i = 0; i < e.attributes.length; ++i)
                {
                    GumboAttribute *attr = static_cast<GumboAttribute *>(e.attributes.data[i]);
                    if (std::string(attr->value).find(selector) != std::string::npos)
                    {
                        match_selector = true;
                        break;
                    }
                }
            }
        }
    }

    if (match_tag && match_selector)
        out.push_back(node);

    // Recursively search children
    for (size_t i = 0; i < e.children.length; ++i)
        find_nodes((GumboNode *)e.children.data[i], tag, selector, out);
}
// Function to extract text from a node
std::string extract_text(GumboNode *node)
{
    if (!node)
        return "";
    if (node->type == GUMBO_NODE_TEXT)
        return node->v.text.text;
    if (node->type != GUMBO_NODE_ELEMENT)
        return "";

    std::string s;
    GumboElement &e = node->v.element;
    for (size_t i = 0; i < e.children.length; ++i)
    {
        std::string t = extract_text((GumboNode *)e.children.data[i]);
        if (!t.empty())
        {
            if (!s.empty())
                s += ' ';
            s += t;
        }
    }

    return s;
}

// Function to extract an attribute from a node
std::string extract_attr(GumboNode *node, const std::string &name)
{
    if (!node || node->type != GUMBO_NODE_ELEMENT)
        return "";
    GumboAttribute *a = gumbo_get_attribute(&node->v.element.attributes, name.c_str());
    return a ? a->value : std::string();
}

// Function to extract URL from a node (checks for href attributes or nested a tags)
std::string extract_url(GumboNode *node, const std::string &base_url)
{
    if (!node)
        return "";

    // Check if this node is an anchor with href
    std::string href = extract_attr(node, "href");
    if (!href.empty())
        return normalize_url(href, base_url);

    // Search for first anchor tag within this node
    std::vector<GumboNode *> anchors;
    find_nodes(node, "a", "", anchors);
    if (!anchors.empty())
    {
        href = extract_attr(anchors[0], "href");
        if (!href.empty())
            return normalize_url(href, base_url);
    }

    return "";
}

// Function to get the current timestamp in ISO format
std::string now_iso()
{
    auto t = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(t);
    std::ostringstream o;
    o << std::put_time(std::localtime(&tt), "%Y-%m-%d %H:%M:%S");
    return o.str();
}

// Function to clean and normalize text content
std::string clean_text(const std::string &text)
{
    std::string result;
    bool space = false;

    for (char c : text)
    {
        if (std::isspace(c))
        {
            if (!space && !result.empty())
            {
                result += ' ';
                space = true;
            }
        }
        else
        {
            result += c;
            space = false;
        }
    }

    // Trim trailing space if present
    if (!result.empty() && result.back() == ' ')
        result.pop_back();

    return result;
}

// URL encode function for creating search URLs
std::string url_encode(const std::string &value)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        throw ScraperException("Failed to initialize CURL for URL encoding");

    char *output = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.length()));
    if (!output)
    {
        curl_easy_cleanup(curl);
        throw ScraperException("Failed to URL encode string: " + value);
    }

    std::string result(output);
    curl_free(output);
    curl_easy_cleanup(curl);

    return result;
}

// Function to replace placeholders in URL templates
std::string format_url(const std::string &url_template,
                       const std::string &job_title,
                       const std::string &location)
{
    std::string url = url_template;

    // Replace {job_title} with URL-encoded job title
    size_t pos = url.find("{job_title}");
    if (pos != std::string::npos)
    {
        url.replace(pos, 11, url_encode(job_title));
    }

    // Replace {location} with URL-encoded location
    pos = url.find("{location}");
    if (pos != std::string::npos)
    {
        url.replace(pos, 10, url_encode(location));
    }

    return url;
}

// Function to extract job listing details from a node
json scrape_details(GumboNode *n, const SiteConfig &cfg, const SearchConfig &search_cfg)
{
    json j;
    j["source"] = cfg.name;
    j["scraped_at"] = now_iso();

    // Extract title
    if (!cfg.title_tag.empty())
    {
        std::vector<GumboNode *> nodes;
        find_nodes(n, cfg.title_tag, cfg.title_class, nodes);
        if (!nodes.empty())
        {
            j["title"] = clean_text(extract_text(nodes[0]));
        }
    }

    // Extract location
    if (!cfg.location_tag.empty())
    {
        std::vector<GumboNode *> nodes;
        find_nodes(n, cfg.location_tag, cfg.location_class, nodes);
        if (!nodes.empty())
        {
            j["location"] = clean_text(extract_text(nodes[0]));
        }
        else
        {
            // Use search location if we couldn't find it in the listing
            j["location"] = search_cfg.location;
        }
    }
    else
    {
        // Default to search location
        j["location"] = search_cfg.location;
    }

    // Extract company (optional for your format)
    if (!cfg.company_tag.empty())
    {
        std::vector<GumboNode *> nodes;
        find_nodes(n, cfg.company_tag, cfg.company_class, nodes);
        if (!nodes.empty())
        {
            j["company"] = clean_text(extract_text(nodes[0]));
        }
    }

    // Extract description
    if (!cfg.description_tag.empty())
    {
        std::vector<GumboNode *> nodes;
        find_nodes(n, cfg.description_tag, cfg.description_class, nodes);
        if (!nodes.empty())
        {
            j["description"] = clean_text(extract_text(nodes[0]));
        }
    }

    // Extract URL
    std::string job_url;
    if (!cfg.url_tag.empty())
    {
        std::vector<GumboNode *> nodes;
        find_nodes(n, cfg.url_tag, cfg.url_class, nodes);
        if (!nodes.empty())
        {
            job_url = extract_url(nodes[0], cfg.base_url);

            // If href is not found directly, try to get it from the "href" attribute
            if (job_url.empty())
            {
                job_url = extract_attr(nodes[0], "href");
                if (!job_url.empty())
                {
                    job_url = normalize_url(job_url, cfg.base_url);
                }
            }
        }
    }
    else
    {
        // Try to extract from title element or container
        job_url = extract_url(n, cfg.base_url);
    }

    if (!job_url.empty())
    {
        j["source"] = job_url;
    }

    // Extract skills - either from a dedicated field or from description
    std::vector<std::string> skills;
    if (!cfg.skills_tag.empty())
    {
        std::vector<GumboNode *> nodes;
        find_nodes(n, cfg.skills_tag, cfg.skills_class, nodes);
        if (!nodes.empty())
        {
            std::string skills_text = clean_text(extract_text(nodes[0]));

            // Basic tokenization assuming comma or bullet separation
            std::istringstream iss(skills_text);
            std::string skill;
            while (std::getline(iss, skill, ','))
            {
                if (!skill.empty())
                {
                    // Clean up whitespace and add to skills array
                    skill = std::regex_replace(skill, std::regex("^\\s+|\\s+$"), "");
                    if (!skill.empty())
                    {
                        skills.push_back(skill);
                    }
                }
            }
        }
    }

    // If no skills found in dedicated field and extract_skills is enabled, extract from description

    // Just initialize an empty skills array
    j["skills"] = json::array();

    // If we have skills, add them to the JSON
    if (!skills.empty())
    {
        j["skills"] = skills;
    }
    else
    {
        // Empty array if no skills found
        j["skills"] = json::array();
    }

    // Apply keyword filtering if specified
    if (!search_cfg.keywords.empty())
    {
        bool match = false;
        std::string description = j.value("description", "");
        std::string title = j.value("title", "");
        std::transform(description.begin(), description.end(), description.begin(), ::tolower);
        std::transform(title.begin(), title.end(), title.begin(), ::tolower);

        for (const auto &keyword : search_cfg.keywords)
        {
            std::string keyword_lower = keyword;
            std::transform(keyword_lower.begin(), keyword_lower.end(), keyword_lower.begin(), ::tolower);

            if (description.find(keyword_lower) != std::string::npos ||
                title.find(keyword_lower) != std::string::npos)
            {
                match = true;
                break;
            }
        }

        if (!match)
        {
            // If no match with keywords, return an empty JSON
            return json();
        }
    }

    return j;
}

// Dice job processor function
void process_dice_jobs(const SiteConfig &site, const SearchConfig &search_cfg,
                       std::vector<json> &all_jobs, int max_jobs)
{
    std::cout << "Scraping from: " << site.name << std::endl;

    try
    {
        // Format search URL with job title and location
        std::string base_search_url = format_url(site.search_url_template,
                                                 search_cfg.job_title,
                                                 search_cfg.location);

        // Process pages
        for (int page = 1; page <= site.max_pages; ++page)
        {
            // Construct pagination URL
            std::string page_url = base_search_url;
            if (!site.pagination_param.empty())
            {
                char separator = (page_url.find('?') != std::string::npos) ? '&' : '?';
                page_url += separator + site.pagination_param + "=" + std::to_string(page);
            }

            std::cout << "  Fetching Dice page " << page << ": " << page_url << std::endl;

            // Fetch page HTML content with a longer delay
            std::this_thread::sleep_for(std::chrono::seconds(5 + (std::rand() % 5)));

            std::string html;
            try
            {
                html = fetch_page(page_url, 3, "Dice");

                // Save HTML for debugging
                std::string debug_path = "debug_dice_raw_" +
                                         std::to_string(page) + "_" +
                                         std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".html";
                std::ofstream debug_file(debug_path);
                if (debug_file.is_open())
                {
                    debug_file << html;
                    debug_file.close();
                    std::cout << "  Saved raw Dice HTML to: " << debug_path << std::endl;
                }
            }
            catch (const ScraperException &e)
            {
                std::cerr << "  Error fetching Dice page: " << e.what() << std::endl;
                break;
            }

            // First check if we can see the job cards
            if (html.find("search-card-wrapper") == std::string::npos &&
                html.find("card-title-link") == std::string::npos)
            {
                std::cout << "  Warning: Dice page doesn't contain expected job card selectors" << std::endl;
                std::cout << "  Examining HTML to find job listing containers..." << std::endl;

                // Look for common patterns that might indicate job listings
                for (const auto &potential_selector : {"job-card", "job-listing", "searchResult", "jobCard"})
                {
                    if (html.find(potential_selector) != std::string::npos)
                    {
                        std::cout << "  Found potential alternative selector: " << potential_selector << std::endl;
                    }
                }
            }

            // Parse HTML with Gumbo
            GumboOutput *output = gumbo_parse(html.c_str());
            if (!output)
            {
                std::cerr << "  Failed to parse HTML for Dice" << std::endl;
                continue;
            }

            // Try multiple possible container selectors
            std::vector<std::pair<std::string, std::string>> container_selectors = {
                {"a", "job-search-job-detail-link"}, // New primary selector
                {"div", "search-card-wrapper"},
                {"div", "job-card"},
                {"div", "card-body"},
                {"div", "jobCard"},
                {"li", "jobsList-item"},
                {"dhi-search-card", ""}};

            std::vector<GumboNode *> containers;

            for (const auto &selector : container_selectors)
            {
                find_nodes(output->root, selector.first, selector.second, containers);
                if (!containers.empty())
                {
                    std::cout << "  Found " << containers.size() << " Dice job listings with selector: "
                              << selector.first << "." << selector.second << std::endl;
                    break;
                }
            }

            // If still empty, try a more generic approach
            if (containers.empty())
            {
                // Find all divs with a height and examine them
                std::vector<GumboNode *> divs;
                find_nodes(output->root, "div", "", divs);

                for (auto *div : divs)
                {
                    // Check if this div might be a job card
                    std::string class_attr = extract_attr(div, "class");
                    std::string id_attr = extract_attr(div, "id");

                    if ((class_attr.find("card") != std::string::npos ||
                         class_attr.find("job") != std::string::npos ||
                         id_attr.find("job") != std::string::npos) &&
                        class_attr.find("container") == std::string::npos)
                    {
                        containers.push_back(div);
                    }
                }

                if (!containers.empty())
                {
                    std::cout << "  Found " << containers.size() << " potential Dice job listings using generic detection" << std::endl;
                }
            }

            // Process job containers
            for (auto *container : containers)
            {
                json job = scrape_details(container, site, search_cfg);

                // Only process valid jobs
                if (!job.empty())
                {
                    // Get the job URL for fetching detailed information
                    // Get the job URL for fetching detailed information
                    std::string job_url = job["source"];

                    // If the URL doesn't start with http, make it absolute
                    if (!job_url.empty() && job_url.find("http") != 0)
                    {
                        job_url = normalize_url(job_url, site.base_url);
                    }

                    if (!job_url.empty())
                    {
                        // Fetch detailed job information
                        json detailed_info = fetch_dice_job_details(job_url, site, search_cfg);

                        // Merge detailed info with basic job info
                        for (auto it = detailed_info.begin(); it != detailed_info.end(); ++it)
                        {
                            job[it.key()] = it.value();
                        }
                    }
                    all_jobs.push_back(job);

                    // Print basic info about the job
                    std::cout << "  Scraped: "
                              << job.value("title", "Unknown Title") << " at "
                              << job.value("company", "Unknown Company") << " in "
                              << job.value("location", "Unknown Location") << std::endl;

                    // Break if we've reached the maximum number of jobs
                    if (all_jobs.size() >= static_cast<size_t>(max_jobs))
                    {
                        std::cout << "  Reached maximum job limit (" << max_jobs << ")" << std::endl;
                        break;
                    }
                }

                // Add a short delay between processing jobs
                std::this_thread::sleep_for(std::chrono::milliseconds(500 + (std::rand() % 1000)));
            }

            // Clean up Gumbo parser
            gumbo_destroy_output(&kGumboDefaultOptions, output);

            // Break if we've reached the maximum number of jobs
            if (all_jobs.size() >= static_cast<size_t>(max_jobs))
            {
                break;
            }

            // Add a longer delay between pages
            std::this_thread::sleep_for(std::chrono::seconds(2 + (std::rand() % 2)));
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error scraping Dice: " << e.what() << std::endl;
    }
}

// Specialized function for SimplyHired
void process_simplyhired_jobs(const SiteConfig &site, const SearchConfig &search_cfg,
                              std::vector<json> &all_jobs, int max_jobs)
{
    std::cout << "Scraping from: " << site.name << std::endl;

    try
    {
        // Format search URL with job title and location
        std::string base_search_url = format_url(site.search_url_template,
                                                 search_cfg.job_title,
                                                 search_cfg.location);

        // Process multiple pages up to max_pages
        for (int page = 1; page <= site.max_pages; ++page)
        {
            // Construct pagination URL
            std::string page_url = base_search_url;
            if (!site.pagination_param.empty())
            {
                char separator = (page_url.find('?') != std::string::npos) ? '&' : '?';
                page_url += separator + site.pagination_param + "=" + std::to_string(page);
            }

            std::cout << "  Fetching page " << page << ": " << page_url << std::endl;

            // Add random delay before request
            int delay_ms = 1000 + (std::rand() % 2000);
            std::cout << "  Waiting for " << delay_ms / 1000.0 << " seconds before request..." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

            // Fetch page HTML content
            std::string html;
            try
            {
                html = fetch_page(page_url, 3, site.name);

                // Save HTML for debugging
                std::string debug_path = "debug_" + site.name + "_page" + std::to_string(page) + "_" +
                                         std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".html";
                std::ofstream debug_file(debug_path);
                if (debug_file.is_open())
                {
                    debug_file << html;
                    debug_file.close();
                    std::cout << "  Saved " << site.name << " HTML to: " << debug_path << std::endl;
                }
            }
            catch (const ScraperException &e)
            {
                std::cerr << "  Error fetching page: " << e.what() << std::endl;
                break;
            }

            // Parse HTML with Gumbo
            GumboOutput *output = gumbo_parse(html.c_str());
            if (!output)
            {
                std::cerr << "  Failed to parse HTML for " << site.name << std::endl;
                continue;
            }

            // Find job listing containers
            std::vector<GumboNode *> containers;
            find_nodes(output->root, site.container_tag, site.container_class, containers);

            std::cout << "  Found " << containers.size() << " job listings" << std::endl;

            // If no containers found, try alternative selectors
            if (containers.empty())
            {
                // Try alternative selectors for SimplyHired
                const std::vector<std::pair<std::string, std::string>> alt_selectors = {
                    {"div", "css-dy1hfy"},
                    {"div", "SerpJob-jobCard"},
                    {"div", "jobCard"},
                    {"li", "job-list-item"}};

                for (const auto &selector : alt_selectors)
                {
                    find_nodes(output->root, selector.first, selector.second, containers);
                    if (!containers.empty())
                    {
                        std::cout << "  Found " << containers.size() << " job listings with alternative selector: "
                                  << selector.first << "." << selector.second << std::endl;
                        break;
                    }
                }
            }

            // If we still don't have containers, try to find job links directly
            if (containers.empty())
            {
                std::vector<GumboNode *> job_links;
                find_nodes(output->root, "a", "chakra-button css-1djbb1k", job_links);

                if (!job_links.empty())
                {
                    std::cout << "  Found " << job_links.size() << " job links directly" << std::endl;

                    // Process each job link
                    for (auto *link : job_links)
                    {
                        std::string job_url = extract_url(link, site.base_url);
                        std::string title = clean_text(extract_text(link));

                        if (!job_url.empty() && !title.empty())
                        {
                            // Create a basic job entry
                            json job;
                            job["title"] = title;
                            job["source"] = job_url;
                            job["scraped_at"] = now_iso();

                            // Fetch detailed job information
                            json detailed_info = fetch_simplyhired_job_details(job_url, site, search_cfg);

                            // Merge detailed info with basic job info
                            for (auto it = detailed_info.begin(); it != detailed_info.end(); ++it)
                            {
                                job[it.key()] = it.value();
                            }

                            all_jobs.push_back(job);

                            std::cout << "  Scraped: " << title << std::endl;

                            // Break if we've reached the maximum number of jobs
                            if (all_jobs.size() >= static_cast<size_t>(max_jobs))
                            {
                                std::cout << "  Reached maximum job limit (" << max_jobs << ")" << std::endl;
                                break;
                            }

                            // Add delay between job processing
                            std::this_thread::sleep_for(std::chrono::milliseconds(1000 + (std::rand() % 2000)));
                        }
                    }
                }
            }
            else
            {
                // Process containers normally
                for (auto *container : containers)
                {
                    // Get job information
                    json job = scrape_details(container, site, search_cfg);

                    // Only process valid jobs
                    if (!job.empty())
                    {
                        // Get the job URL for fetching detailed information
                        std::string job_url = job["source"];

                        if (!job_url.empty())
                        {
                            // Fetch detailed job information
                            json detailed_info = fetch_simplyhired_job_details(job_url, site, search_cfg);

                            // Merge detailed info with basic job info
                            for (auto it = detailed_info.begin(); it != detailed_info.end(); ++it)
                            {
                                job[it.key()] = it.value();
                            }
                        }

                        all_jobs.push_back(job);

                        // Print basic info about the job
                        std::cout << "  Scraped: "
                                  << job.value("title", "Unknown Title") << " at "
                                  << job.value("company", "Unknown Company") << " in "
                                  << job.value("location", "Unknown Location") << std::endl;

                        // Break if we've reached the maximum number of jobs
                        if (all_jobs.size() >= static_cast<size_t>(max_jobs))
                        {
                            std::cout << "  Reached maximum job limit (" << max_jobs << ")" << std::endl;
                            break;
                        }
                    }

                    // Add a small delay between processing jobs
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000 + (std::rand() % 2000)));
                }
            }

            // Clean up Gumbo parser
            gumbo_destroy_output(&kGumboDefaultOptions, output);

            // Break if we've reached the maximum number of jobs
            if (all_jobs.size() >= static_cast<size_t>(max_jobs))
            {
                break;
            }

            // Respect the site's delay between requests to avoid being blocked
            std::this_thread::sleep_for(site.delay + std::chrono::milliseconds(std::rand() % 5000));
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error scraping " << site.name << ": " << e.what() << std::endl;
    }
}

// LinkedIn config update
SiteConfig create_linkedin_config()
{
    return {
        "LinkedIn",
        "https://www.linkedin.com",
        "https://www.linkedin.com/jobs/search?keywords={job_title}&location={location}&f_TPR=r86400", // Last 24 hours
        "div", "base-card relative",
        "h3", "base-search-card__title",
        "h4", "base-search-card__subtitle",
        "span", "job-search-card__location",
        "div", "jobs-description-content",
        "a", "base-card__full-link",
        "time", "",
        "", "",
        "start",
        2,
        std::chrono::seconds(3)};
}

// SimplyHired updated config
// Update your SimplyHired config with this version
// Updated SimplyHired config that targets the specific elements you've shown
SiteConfig create_simplyhired_config()
{
    return {
        "SimplyHired",
        "https://www.simplyhired.com",
        "https://www.simplyhired.com/search?q={job_title}&l={location}",
        "div", "searchSerpJob",                        // This matches the job card container
        "a", "chakra-button css-1djbb1k",              // This exactly matches the link you provided
        "span", "companyName",                         // Company name element
        "span", "searchSerpJobLocation",               // Location element
        "div", "viewJobBodyJobFullDescriptionContent", // This matches the content div in your example
        "a", "chakra-button css-1djbb1k",              // Same link for URL extraction
        "p", "css-5yilgw",                             // Date stamp
        "", "",
        "pn",
        2,
        6s};
}

// Dice updated config
// Updated Dice config
SiteConfig create_dice_config()
{
    return {
        "Dice",
        "https://www.dice.com",
        "https://www.dice.com/jobs?q={job_title}&location={location}",
        "a", "job-search-job-detail-link", // Updated selector based on the provided HTML
        "a", "job-search-job-detail-link", // Title is in the same element
        "div", "company-name-rating",      // This might need adjustment based on actual HTML
        "div", "location",                 // This might need adjustment based on actual HTML
        "div", "jobDescriptionHtml",       // Based on your HTML snippet
        "a", "job-search-job-detail-link", // Using the same link element for URL
        "div", "posted-date",              // May need adjustment
        "", "",
        "page",
        2,
        5s};
}

json fetch_linkedin_job_details(const std::string &job_url, const SiteConfig &site_config, const SearchConfig &search_cfg)
{
    json job_details;

    try
    {
        std::cout << "  Fetching detailed job information from: " << job_url << std::endl;

        // Add delay before fetching to avoid rate limiting
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // Fetch the job detail page - USING THE SPECIALIZED LINKEDIN FUNCTION
        std::string html = fetch_linkedin_page(job_url);

        // Parse HTML with Gumbo
        GumboOutput *output = gumbo_parse(html.c_str());
        if (!output)
        {
            std::cerr << "  Failed to parse job detail HTML" << std::endl;
            return job_details;
        }

        // Look for the job description container using multiple possible selectors
        std::vector<GumboNode *> description_containers;

        // Try all these selectors one by one
        const std::vector<std::pair<std::string, std::string>> selectors = {
            {"div", "jobs-description-content"},
            {"div", "jobs-box__html-content"},
            {"div", "description__text"},
            {"div", "show-more-less-html__markup"},
            {"div", "jobs-description__content"},
            {"section", "description"},
            {"div", "job-detail-body"},
            {"div", "job-description"},
            {"div", "job-view-layout jobs-details"}};

        // Try each selector until we find something
        for (const auto &selector : selectors)
        {
            find_nodes(output->root, selector.first, selector.second, description_containers);
            if (!description_containers.empty())
            {
                std::cout << "  Found description using selector: " << selector.first << "." << selector.second << std::endl;
                break;
            }
        }

        // If still empty, try a more generic approach to find any large text block
        if (description_containers.empty())
        {
            std::cout << "  Trying generic approach to find description..." << std::endl;
            std::vector<GumboNode *> divs;
            find_nodes(output->root, "div", "", divs);

            // Find the div with the most text content (likely the description)
            size_t max_length = 0;
            GumboNode *best_candidate = nullptr;

            for (auto *div : divs)
            {
                std::string content = extract_text(div);
                if (content.length() > max_length && content.length() > 100)
                {
                    max_length = content.length();
                    best_candidate = div;
                }
            }

            if (best_candidate)
            {
                description_containers.push_back(best_candidate);
                std::cout << "  Found potential description by content length: " << max_length << " chars" << std::endl;
            }
        }

        // Extract description if found
        if (!description_containers.empty())
        {
            // Extract the full description text
            std::string description = clean_text(extract_text(description_containers[0]));
            job_details["description"] = description;

            // Extract skills if enabled
            job_details["skills"] = json::array();

            std::cout << "  Successfully extracted description (" << description.length() << " chars)" << std::endl;
        }
        else
        {
            std::cerr << "  Could not find job description container" << std::endl;

            // Save the HTML for debugging
            std::string debug_path = "debug_linkedin_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".html";
            std::ofstream debug_file(debug_path);
            if (debug_file.is_open())
            {
                debug_file << html;
                debug_file.close();
                std::cout << "  Saved HTML for debugging to: " << debug_path << std::endl;
            }
        }

        // Clean up Gumbo parser
        gumbo_destroy_output(&kGumboDefaultOptions, output);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error fetching job details: " << e.what() << std::endl;
    }

    return job_details;
}

// Simplified LinkedIn-specific fetch function

void process_linkedin_jobs(const SiteConfig &site, const SearchConfig &search_cfg,
                           std::vector<json> &all_jobs, int max_jobs)
{
    std::cout << "Scraping from: " << site.name << std::endl;

    try
    {
        // Format search URL with job title and location
        std::string base_search_url = format_url(site.search_url_template,
                                                 search_cfg.job_title,
                                                 search_cfg.location);

        // Process multiple pages up to max_pages
        for (int page = 1; page <= site.max_pages; ++page)
        {
            // Construct pagination URL
            std::string page_url = base_search_url;
            if (!site.pagination_param.empty())
            {
                char separator = (page_url.find('?') != std::string::npos) ? '&' : '?';
                page_url += separator + site.pagination_param + "=" + std::to_string(page);
            }

            std::cout << "  Fetching LinkedIn page " << page << ": " << page_url << std::endl;

            // Fetch page HTML content - USING THE SPECIALIZED LINKEDIN FUNCTION
            std::string html;
            try
            {
                html = fetch_linkedin_page(page_url);
            }
            catch (const ScraperException &e)
            {
                std::cerr << "  Error fetching LinkedIn page: " << e.what() << std::endl;
                break;
            }

            // Parse HTML with Gumbo
            GumboOutput *output = gumbo_parse(html.c_str());
            if (!output)
            {
                std::cerr << "  Failed to parse HTML for LinkedIn" << std::endl;
                continue;
            }

            // Find job listing containers
            std::vector<GumboNode *> containers;
            find_nodes(output->root, site.container_tag, site.container_class, containers);

            std::cout << "  Found " << containers.size() << " LinkedIn job listings" << std::endl;

            // Extract job details from each container
            for (auto *container : containers)
            {
                // Get basic job information
                json job = scrape_details(container, site, search_cfg);

                // Only process valid jobs
                if (!job.empty())
                {
                    // Get the job URL for fetching detailed information
                    std::string job_url = job["source"];

                    if (!job_url.empty())
                    {
                        // Also update this to use fetch_linkedin_page:
                        json detailed_info;
                        try
                        {
                            std::cout << "  Fetching LinkedIn job details from: " << job_url << std::endl;
                            // Add delay before fetching to avoid rate limiting
                            std::this_thread::sleep_for(std::chrono::seconds(2));

                            // Fetch detailed job information using the specialized function
                            std::string detail_html = fetch_linkedin_page(job_url);

                            // Parse the details (you can keep using your existing detailed parsing logic)
                            GumboOutput *detail_output = gumbo_parse(detail_html.c_str());
                            if (detail_output)
                            {
                                // Look for the job description container
                                std::vector<GumboNode *> description_containers;

                                // Try these selectors one by one
                                const std::vector<std::pair<std::string, std::string>> selectors = {
                                    {"div", "jobs-description-content"},
                                    {"div", "jobs-box__html-content"},
                                    {"div", "description__text"},
                                    {"div", "show-more-less-html__markup"},
                                    {"div", "jobs-description__content"},
                                    {"section", "description"},
                                    {"div", "job-detail-body"},
                                    {"div", "job-description"}};

                                for (const auto &selector : selectors)
                                {
                                    find_nodes(detail_output->root, selector.first, selector.second, description_containers);
                                    if (!description_containers.empty())
                                        break;
                                }

                                if (!description_containers.empty())
                                {
                                    std::string description = clean_text(extract_text(description_containers[0]));
                                    detailed_info["description"] = description;
                                }

                                gumbo_destroy_output(&kGumboDefaultOptions, detail_output);
                            }
                        }
                        catch (const std::exception &e)
                        {
                            std::cerr << "  Error fetching LinkedIn job details: " << e.what() << std::endl;
                        }

                        // Merge detailed info with basic job info
                        for (auto it = detailed_info.begin(); it != detailed_info.end(); ++it)
                        {
                            job[it.key()] = it.value();
                        }
                    }

                    all_jobs.push_back(job);

                    // Print basic info about the job
                    std::cout << "  Scraped LinkedIn job: "
                              << job.value("title", "Unknown Title") << " at "
                              << job.value("company", "Unknown Company") << " in "
                              << job.value("location", "Unknown Location") << std::endl;

                    // Break if we've reached the maximum number of jobs
                    if (all_jobs.size() >= static_cast<size_t>(max_jobs))
                    {
                        std::cout << "  Reached maximum job limit (" << max_jobs << ")" << std::endl;
                        break;
                    }
                }

                // Add a small delay between processing jobs
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            // Clean up Gumbo parser
            gumbo_destroy_output(&kGumboDefaultOptions, output);

            // Break if we've reached the maximum number of jobs
            if (all_jobs.size() >= static_cast<size_t>(max_jobs))
            {
                break;
            }

            // Respect the site's delay between requests
            std::this_thread::sleep_for(site.delay);
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error scraping LinkedIn: " << e.what() << std::endl;
    }
}
// Updated function for fetch_simplyhired_job_details
json fetch_simplyhired_job_details(const std::string &job_url, const SiteConfig &site_config, const SearchConfig &search_cfg)
{
    json job_details;

    try
    {
        std::cout << "  Fetching SimplyHired job details from: " << job_url << std::endl;

        // Add delay before request
        int delay_ms = 2000 + (std::rand() % 4000);
        std::cout << "  Waiting for " << delay_ms / 1000.0 << " seconds before request..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

        // Fetch the job detail page
        std::string html = fetch_page(job_url, 3, "SimplyHired");

        // Save the full HTML for debugging
        std::string debug_path = "debug_simplyhired_" +
                                 std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".html";
        std::ofstream debug_file(debug_path);
        if (debug_file.is_open())
        {
            debug_file << html;
            debug_file.close();
            std::cout << "  Saved SimplyHired HTML to: " << debug_path << " for analysis" << std::endl;
        }

        // Parse HTML with Gumbo
        GumboOutput *output = gumbo_parse(html.c_str());
        if (!output)
        {
            std::cerr << "  Failed to parse SimplyHired job detail HTML" << std::endl;
            return job_details;
        }

        // First, look for company info
        std::vector<GumboNode *> company_nodes;
        find_nodes(output->root, "span", "companyName", company_nodes);
        if (!company_nodes.empty())
        {
            job_details["company"] = clean_text(extract_text(company_nodes[0]));
        }

        // Look for location info
        std::vector<GumboNode *> location_nodes;
        find_nodes(output->root, "span", "jobLocation", location_nodes);
        if (!location_nodes.empty())
        {
            job_details["location"] = clean_text(extract_text(location_nodes[0]));
        }

        // Updated selector for description based on your example
        std::vector<GumboNode *> description_containers;
        find_nodes(output->root, "div", "viewJobBodyJobFullDescriptionContent", description_containers);

        if (!description_containers.empty())
        {
            std::cout << "  Found SimplyHired description with primary selector" << std::endl;
            std::string description = clean_text(extract_text(description_containers[0]));
            job_details["description"] = description;
            std::cout << "  Successfully extracted SimplyHired description (" << description.length() << " chars)" << std::endl;
        }
        else
        {
            // Try alternative selectors if primary not found
            const std::vector<std::pair<std::string, std::string>> alt_selectors = {
                {"div", "css-cxpe4v"},
                {"div", "jobDescriptionSection"},
                {"div", "chakra-stack css-yfgykh"},
                {"section", "viewjob-content"}};

            for (const auto &selector : alt_selectors)
            {
                find_nodes(output->root, selector.first, selector.second, description_containers);
                if (!description_containers.empty())
                {
                    std::cout << "  Found SimplyHired description using alternative selector: "
                              << selector.first << "." << selector.second << std::endl;
                    std::string description = clean_text(extract_text(description_containers[0]));
                    job_details["description"] = description;
                    std::cout << "  Successfully extracted SimplyHired description (" << description.length() << " chars)" << std::endl;
                    break;
                }
            }
        }

        // If still no description, try generic approach
        if (!job_details.contains("description") || job_details["description"].get<std::string>().empty())
        {
            std::vector<GumboNode *> divs;
            find_nodes(output->root, "div", "", divs);

            size_t max_length = 100;
            GumboNode *best_candidate = nullptr;

            for (auto *div : divs)
            {
                std::string content = extract_text(div);
                if (content.length() > max_length)
                {
                    max_length = content.length();
                    best_candidate = div;
                }
            }

            if (best_candidate)
            {
                std::string description = clean_text(extract_text(best_candidate));
                job_details["description"] = description;
                std::cout << "  Found potential SimplyHired description by length: " << max_length << " chars" << std::endl;
            }
        }

        // Extract skills if we have a description and extraction is enabled

        // Clean up Gumbo parser
        gumbo_destroy_output(&kGumboDefaultOptions, output);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error fetching SimplyHired job details: " << e.what() << std::endl;
    }

    return job_details;
}

// Special function to fetch Dice job details
// Improved fetch_dice_job_details function
json fetch_dice_job_details(const std::string &job_url, const SiteConfig &site_config, const SearchConfig &search_cfg)
{
    json job_details;
    static int failure_count = 0;
    static int success_count = 0;
    static bool reset_session = false;

    try
    {
        // Check if we need to reset the session due to too many failures
        if (failure_count > 3 && success_count < 1)
        {
            std::cout << "  Too many consecutive Dice failures. Resetting session..." << std::endl;
            reset_session = true;
            failure_count = 0;

            // Sleep for a longer period to reset the session
            std::this_thread::sleep_for(std::chrono::minutes(2));
        }

        std::cout << "  Fetching Dice job details from: " << job_url << std::endl;

        // Add longer delay for Dice
        std::this_thread::sleep_for(std::chrono::seconds(4 + (std::rand() % 4)));

        // Special headers for Dice
        std::vector<std::string> dice_user_agents = {
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/123.0.0.0 Safari/537.36",
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.4 Safari/605.1.15",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:124.0) Gecko/20100101 Firefox/124.0"};

        // Initialize CURL for Dice
        CURL *curl = curl_easy_init();
        std::string buffer;

        if (curl)
        {
            curl_easy_setopt(curl, CURLOPT_URL, job_url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

            // Set a very browser-like user agent for Dice
            std::string user_agent = dice_user_agents[std::rand() % dice_user_agents.size()];
            curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());

            // Create cookies that look more like a real browser session
            std::string session_id = generate_random_string(32);
            std::string visitor_id = generate_random_string(16);
            std::string dice_cookie = "dice.search-id=" + session_id +
                                      "; dice.visitor-id=" + visitor_id +
                                      "; dice.session-started=true";

            // Set up headers for Dice
            struct curl_slist *headers = NULL;
            headers = curl_slist_append(headers, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8");
            headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.5");
            headers = curl_slist_append(headers, "Connection: keep-alive");
            headers = curl_slist_append(headers, "Upgrade-Insecure-Requests: 1");
            headers = curl_slist_append(headers, "Cache-Control: max-age=0");
            headers = curl_slist_append(headers, "Sec-Fetch-Dest: document");
            headers = curl_slist_append(headers, "Sec-Fetch-Mode: navigate");
            headers = curl_slist_append(headers, "Sec-Fetch-Site: same-origin");
            headers = curl_slist_append(headers, "Sec-Fetch-User: ?1");
            headers = curl_slist_append(headers, ("Cookie: " + dice_cookie).c_str());
            headers = curl_slist_append(headers, "Referer: https://www.dice.com/jobs");

            // Create a device that looks more like a real browser
            headers = curl_slist_append(headers, "Sec-CH-UA: \"Google Chrome\";v=\"113\", \"Chromium\";v=\"113\"");
            headers = curl_slist_append(headers, "Sec-CH-UA-Mobile: ?0");
            headers = curl_slist_append(headers, "Sec-CH-UA-Platform: \"Windows\"");

            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

            // Timeout settings
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

            // Execute request
            CURLcode res = curl_easy_perform(curl);

            if (res == CURLE_OK)
            {
                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

                if (http_code >= 200 && http_code < 300)
                {
                    // Save the response for debugging
                    std::string debug_path = "debug_dice_success_" +
                                             std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".html";
                    std::ofstream debug_file(debug_path);
                    if (debug_file.is_open())
                    {
                        debug_file << buffer;
                        debug_file.close();
                        std::cout << "  Saved successful Dice response to: " << debug_path << std::endl;
                    }

                    // Process the HTML as normal
                    // ... (parse HTML with Gumbo, extract description, etc.)

                    success_count++;
                    failure_count = 0; // Reset failure count on success
                }
                else
                {
                    std::cerr << "  Dice HTTP error: " << http_code << std::endl;

                    // Save error response for debugging
                    std::string error_path = "debug_dice_error_" +
                                             std::to_string(http_code) + "_" +
                                             std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".html";
                    std::ofstream error_file(error_path);
                    if (error_file.is_open())
                    {
                        error_file << buffer;
                        error_file.close();
                        std::cout << "  Saved Dice error response to: " << error_path << std::endl;
                    }

                    failure_count++;
                }
            }
            else
            {
                std::cerr << "  Dice CURL error: " << curl_easy_strerror(res) << std::endl;
                failure_count++;
            }

            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }

        // Parse HTML with Gumbo
        GumboOutput *output = gumbo_parse(buffer.c_str());
        if (!output)
        {
            std::cerr << "  Failed to parse Dice job detail HTML" << std::endl;
            failure_count++;
            return job_details;
        }

        // Try multiple possible description selectors for Dice
        // Try multiple possible description selectors for Dice
        const std::vector<std::pair<std::string, std::string>> description_selectors = {
            {"div", "jobDescriptionHtml"}, // New primary selector
            {"div", "job-description"},
            {"div", "jobdescription"},
            {"div", "job-details-description"},
            {"div", "jobDescription"},
            {"div", "job-overview"},
            {"div", "job-info"},
            {"div", "description"}};

        std::vector<GumboNode *> description_containers;

        for (const auto &selector : description_selectors)
        {
            find_nodes(output->root, selector.first, selector.second, description_containers);
            if (!description_containers.empty())
            {
                std::cout << "  Found Dice description using: "
                          << selector.first << "." << selector.second << std::endl;
                break;
            }
        }

        // If still empty, try a more generic approach
        if (description_containers.empty())
        {
            // Look for job ID in URL to help find content
            std::string job_id;
            size_t id_pos = job_url.find("/job/detail/");
            if (id_pos != std::string::npos)
            {
                id_pos += 12; // Length of "/job/detail/"
                size_t id_end = job_url.find("/", id_pos);
                if (id_end != std::string::npos)
                {
                    job_id = job_url.substr(id_pos, id_end - id_pos);
                    std::cout << "  Extracted Dice job ID: " << job_id << std::endl;

                    // Try to find elements specifically related to this job ID
                    std::vector<GumboNode *> divs;
                    find_nodes(output->root, "div", "", divs);

                    for (auto *div : divs)
                    {
                        std::string id_attr = extract_attr(div, "id");
                        std::string class_attr = extract_attr(div, "class");

                        if ((id_attr.find(job_id) != std::string::npos ||
                             id_attr.find("job-detail") != std::string::npos) ||
                            (class_attr.find("job-detail") != std::string::npos ||
                             class_attr.find("description") != std::string::npos))
                        {
                            description_containers.push_back(div);
                            std::cout << "  Found Dice description container by job ID or class" << std::endl;
                            break;
                        }
                    }
                }
            }

            // If still not found, try finding the largest text block
            if (description_containers.empty())
            {
                std::vector<GumboNode *> divs;
                find_nodes(output->root, "div", "", divs);

                size_t max_length = 200; // Higher threshold for Dice
                GumboNode *best_candidate = nullptr;

                for (auto *div : divs)
                {
                    std::string content = extract_text(div);
                    if (content.length() > max_length)
                    {
                        max_length = content.length();
                        best_candidate = div;
                    }
                }

                if (best_candidate)
                {
                    description_containers.push_back(best_candidate);
                    std::cout << "  Found potential Dice description by length: " << max_length << " chars" << std::endl;
                }
            }
        }

        // Extract description if found
        if (!description_containers.empty())
        {
            std::string description = clean_text(extract_text(description_containers[0]));
            job_details["description"] = description;

            std::cout << "  Successfully extracted Dice description (" << description.length() << " chars)" << std::endl;
            success_count++;
            failure_count = 0; // Reset failure count on success
        }
        else
        {
            std::cerr << "  Could not find Dice job description container" << std::endl;
            failure_count++;
        }

        // Clean up Gumbo parser
        gumbo_destroy_output(&kGumboDefaultOptions, output);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error fetching Dice job details: " << e.what() << std::endl;
        failure_count++;
    }

    return job_details;
}

// Function to save scraped jobs to JSON file
void save_to_json(const std::vector<json> &jobs, const std::string &filepath)
{
    std::cout << "Attempting to save " << jobs.size() << " jobs to: " << filepath << std::endl;

    std::ofstream file(filepath);
    if (!file.is_open())
    {
        std::cerr << "Failed to open JSON file for writing: " << filepath << std::endl;
        // Try to create parent directories if they don't exist
        try
        {
            fs::path path(filepath);
            fs::create_directories(path.parent_path());
            file.open(filepath);
            if (!file.is_open())
            {
                std::cerr << "Still can't open file after creating directories" << std::endl;
                return;
            }
        }
        catch (const fs::filesystem_error &e)
        {
            std::cerr << "Error creating directories: " << e.what() << std::endl;
            return;
        }
    }

    try
    {
        file << json(jobs).dump(4);
        file.close();
        std::cout << "Successfully saved " << jobs.size() << " jobs to " << filepath << std::endl;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error during JSON serialization: " << e.what() << std::endl;
    }
}

// Helper function to deduplicate jobs based on title and company
std::vector<json> deduplicate_jobs(const std::vector<json> &jobs)
{
    std::vector<json> unique_jobs;
    std::set<std::string> seen_fingerprints;

    for (const auto &job : jobs)
    {
        // Create a fingerprint based on title and company
        std::string title = job.value("title", "");
        std::string company = job.value("company", "");
        std::transform(title.begin(), title.end(), title.begin(), ::tolower);
        std::transform(company.begin(), company.end(), company.begin(), ::tolower);

        // Simple fingerprint using title and company
        std::string fingerprint = title + "|" + company;

        // Only add if we haven't seen this fingerprint before
        if (seen_fingerprints.find(fingerprint) == seen_fingerprints.end())
        {
            seen_fingerprints.insert(fingerprint);
            unique_jobs.push_back(job);
        }
    }

    return unique_jobs;
}

// Function to export jobs to CSV format
void save_to_csv(const std::vector<json> &jobs, const std::string &filepath)
{
    std::ofstream file(filepath);
    if (!file.is_open())
    {
        std::cerr << "Failed to open CSV file for writing: " << filepath << std::endl;
        return;
    }

    // Write CSV header
    file << "Title,Company,Location,Description,Source,Source URL,Scraped At,Skills\n";

    // Write job data
    for (const auto &job : jobs)
    {
        // Helper function to escape CSV fields
        auto escape_csv = [](const std::string &s) -> std::string
        {
            if (s.find_first_of(",\"\n") != std::string::npos)
            {
                std::string escaped = s;
                // Replace " with ""
                size_t pos = 0;
                while ((pos = escaped.find("\"", pos)) != std::string::npos)
                {
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
        if (job.contains("skills") && job["skills"].is_array())
        {
            for (const auto &skill : job["skills"])
            {
                if (!skills_str.empty())
                    skills_str += "; ";
                skills_str += skill.get<std::string>();
            }
        }
        file << escape_csv(skills_str) << "\n";
    }

    file.close();
}

#ifdef ENABLE_SQLITE
// SQLite implementation (when ENABLE_SQLITE is defined)
#include <sqlite3.h>

bool init_sqlite_db(const std::string &db_path)
{
    sqlite3 *db;
    char *err_msg = nullptr;

    // Open database connection
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Cannot open database: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        return false;
    }

    // SQL statement to create jobs table if it doesn't exist
    const char *sql =
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
    if (rc != SQLITE_OK)
    {
        std::cerr << "SQL error: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return false;
    }

    // Close database
    sqlite3_close(db);
    return true;
}

bool save_to_sqlite(const std::vector<json> &jobs, const std::string &db_path)
{
    sqlite3 *db;
    char *err_msg = nullptr;
    sqlite3_stmt *stmt;

    // Open database connection
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Cannot open database: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        return false;
    }

    // Begin transaction for better performance
    rc = sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to begin transaction: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return false;
    }

    // Prepare SQL statement
    const char *sql =
        "INSERT INTO jobs (title, company, location, description, source, source_url, scraped_at, skills) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";

    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        return false;
    }

    // Insert all jobs
    for (const auto &job : jobs)
    {
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
        if (job.contains("skills") && job["skills"].is_array())
        {
            for (const auto &skill : job["skills"])
            {
                if (!skills_str.empty())
                    skills_str += ", ";
                skills_str += skill.get<std::string>();
            }
        }
        sqlite3_bind_text(stmt, 8, skills_str.c_str(), -1, SQLITE_STATIC);

        // Execute statement
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE)
        {
            std::cerr << "Failed to insert job: " << sqlite3_errmsg(db) << std::endl;
        }

        // Reset statement for next insertion
        sqlite3_reset(stmt);
    }

    // Finalize statement
    sqlite3_finalize(stmt);

    // Commit transaction
    rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK)
    {
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
std::vector<SiteConfig> initialize_site_configs()
{
    std::vector<SiteConfig> sites;

    // LinkedIn - Use the specialized function
    sites.push_back(create_linkedin_config());

    // SimplyHired - With updated selectors
    sites.push_back(create_simplyhired_config());

    // Dice - With updated selectors
    sites.push_back(create_dice_config());

    return sites;
}

// Print help message function
void print_help(char *program_name)
{
    std::cout << "Job Scraper - Scrapes job listings from popular job sites\n\n"
              << "Usage: " << program_name << " [options]\n\n"
              << "Options:\n"
              << "  --job-title TITLE     Job title to search for (default: Software Developer)\n"
              << "  --location LOCATION   Location to search in (default: Remote)\n"
              << "  --site SITE           Scrape only the specified site (LinkedIn, Indeed, SimplyHired, etc.)\n"
              << "  --output-dir DIR      Set output directory for files (default: ./output)\n"
              << "  --sqlite PATH         Enable SQLite output and set database path\n"
              << "  --interval HOURS      Set scraping interval in hours (default: 1)\n"
              << "  --max-jobs N          Maximum number of jobs to scrape (default: 100)\n"
              << "  --keyword WORD        Add keyword filter (can be used multiple times)\n"
              << "  --no-skills           Disable automatic skill extraction\n"
              << "  --help                Show this help message\n";
}

// Main function
// Main function
int main(int argc, char **argv)
{
    // Initialize CURL globally
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Default configurations
    SearchConfig search_cfg;
    OutputConfig output_cfg;

    // Initialize random number generator
    std::srand(static_cast<unsigned>(std::time(nullptr)));

    // Parse command line arguments
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg == "--job-title" && i + 1 < argc)
        {
            search_cfg.job_title = argv[++i];
        }
        else if (arg == "--location" && i + 1 < argc)
        {
            search_cfg.location = argv[++i];
        }
        else if (arg == "--site" && i + 1 < argc)
        {
            search_cfg.target_site = argv[++i];
            std::cout << "Targeting only site: " << search_cfg.target_site << std::endl;
        }
        else if (arg == "--output-dir" && i + 1 < argc)
        {
            output_cfg.output_dir = argv[++i];
        }
        else if (arg == "--sqlite" && i + 1 < argc)
        {
            output_cfg.sqlite_output = true;
            output_cfg.sqlite_db_path = argv[++i];
        }
        else if (arg == "--interval" && i + 1 < argc)
        {
            output_cfg.scrape_interval = std::chrono::hours(std::stoi(argv[++i]));
        }
        else if (arg == "--max-jobs" && i + 1 < argc)
        {
            output_cfg.max_jobs = std::stoi(argv[++i]);
            std::cout << "Setting max jobs to: " << output_cfg.max_jobs << std::endl;
        }
        else if (arg == "--keyword" && i + 1 < argc)
        {
            search_cfg.keywords.push_back(argv[++i]);
        }

        else if (arg == "--help")
        {
            print_help(argv[0]);
            return 0;
        }
        else
        {
            std::cerr << "Unknown option: " << arg << "\n";
            print_help(argv[0]);
            return 1;
        }
    }

    // Get site configurations
    auto sites = initialize_site_configs();

    // Main scraping loop
    while (true)
    {
        std::cout << "=== Starting job scraping at " << now_iso() << " ===" << std::endl;
        std::cout << "Searching for: " << search_cfg.job_title << " in " << search_cfg.location << std::endl;

        // Collection to store all jobs
        std::vector<json> all_jobs;

        // If not targeting a specific site, randomize the order to avoid patterns
        if (search_cfg.target_site.empty())
        {
            rotate_job_sites(sites);
            std::cout << "Randomized job site processing order" << std::endl;
        }
        int num_active_sites = 0;
        for (const auto &site : sites)
        {
            if (search_cfg.target_site.empty() || site.name == search_cfg.target_site)
            {
                if (site.name == "LinkedIn" || site.name == "SimplyHired" || site.name == "Dice")
                {
                    num_active_sites++;
                }
            }
        }

        // Calculate jobs per site (with a minimum to ensure we get some from each)
        int jobs_per_site = std::max<int>(5, output_cfg.max_jobs / (num_active_sites > 0 ? num_active_sites : 1));
        std::cout << "Distributing approximately " << jobs_per_site << " jobs per site" << std::endl;

        // Track jobs collected per site for logging
        std::map<std::string, int> jobs_collected;

        // Process each job site
        for (const auto &site : sites)
        {
            // Skip sites that don't match the target_site parameter (if specified)
            if (!search_cfg.target_site.empty() && site.name != search_cfg.target_site)
            {
                continue;
            }

            // Skip any sites that aren't the ones we want
            if (site.name != "LinkedIn" && site.name != "SimplyHired" && site.name != "Dice")
            {
                continue;
            }

            // Store the current size before processing this site
            size_t initial_size = all_jobs.size();

            try
            {
                // Use specialized processor for each site
                if (site.name == "LinkedIn")
                {
                    process_linkedin_jobs(site, search_cfg, all_jobs, jobs_per_site);
                }
                else if (site.name == "SimplyHired")
                {
                    process_simplyhired_jobs(site, search_cfg, all_jobs, jobs_per_site);
                }
                else if (site.name == "Dice")
                {
                    process_dice_jobs(site, search_cfg, all_jobs, jobs_per_site);
                }

                // Log how many jobs we got from this site
                jobs_collected[site.name] = all_jobs.size() - initial_size;
                std::cout << "Collected " << jobs_collected[site.name] << " jobs from " << site.name << std::endl;
            }
            catch (const std::exception &e)
            {
                std::cerr << "Error scraping " << site.name << ": " << e.what() << std::endl;
            }

            // Break if we've reached the maximum number of jobs across all sites
            // Keep this as a safety check for the overall limit
            if (all_jobs.size() >= static_cast<size_t>(output_cfg.max_jobs))
            {
                std::cout << "Reached overall maximum job limit (" << output_cfg.max_jobs << ")" << std::endl;
                break;
            }

            // Add a longer delay between different sites to avoid detection
            std::cout << "Adding delay between job sites..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(15 + (std::rand() % 15)));
        }

        // Print summary of jobs collected from each site
        std::cout << "=== Job Collection Summary ===" << std::endl;
        for (const auto &[site_name, count] : jobs_collected)
        {
            std::cout << site_name << ": " << count << " jobs" << std::endl;
        }
        // Process each job site
        for (const auto &site : sites)
        {
            // Skip sites that don't match the target_site parameter (if specified)
            if (!search_cfg.target_site.empty() && site.name != search_cfg.target_site)
            {
                continue;
            }

            try
            {
                // Use specialized processor for each site
                if (site.name == "LinkedIn")
                {
                    process_linkedin_jobs(site, search_cfg, all_jobs, output_cfg.max_jobs);
                }

                else if (site.name == "SimplyHired")
                {
                    process_simplyhired_jobs(site, search_cfg, all_jobs, output_cfg.max_jobs);
                }
                else if (site.name == "Dice")
                {
                    // Add Dice processor function call here when implemented
                    process_dice_jobs(site, search_cfg, all_jobs, output_cfg.max_jobs);
                }
                else
                {
                    // Use the regular processing logic for other sites
                    std::cout << "Scraping from: " << site.name << std::endl;

                    // Format search URL with job title and location
                    std::string base_search_url = format_url(site.search_url_template,
                                                             search_cfg.job_title,
                                                             search_cfg.location);

                    // Process multiple pages up to max_pages
                    for (int page = 1; page <= site.max_pages; ++page)
                    {
                        // Construct pagination URL
                        std::string page_url = base_search_url;
                        if (!site.pagination_param.empty())
                        {
                            char separator = (page_url.find('?') != std::string::npos) ? '&' : '?';
                            page_url += separator + site.pagination_param + "=" + std::to_string(page);
                        }

                        std::cout << "  Fetching page " << page << ": " << page_url << std::endl;

                        // Add random delay before request
                        int delay_ms = 3000 + (std::rand() % 5000);
                        std::cout << "  Waiting for " << delay_ms / 1000.0 << " seconds before request..." << std::endl;
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

                        // Fetch page HTML content
                        std::string html;
                        try
                        {
                            html = fetch_page(page_url, 3, site.name);

                            // Save HTML for debugging
                            std::string debug_path = "debug_" + site.name + "_page" + std::to_string(page) + "_" +
                                                     std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".html";
                            std::ofstream debug_file(debug_path);
                            if (debug_file.is_open())
                            {
                                debug_file << html;
                                debug_file.close();
                                std::cout << "  Saved " << site.name << " HTML to: " << debug_path << std::endl;
                            }
                        }
                        catch (const ScraperException &e)
                        {
                            std::cerr << "  Error fetching page: " << e.what() << std::endl;
                            break;
                        }

                        // Parse HTML with Gumbo
                        GumboOutput *output = gumbo_parse(html.c_str());
                        if (!output)
                        {
                            std::cerr << "  Failed to parse HTML for " << site.name << std::endl;
                            continue;
                        }

                        // Find job listing containers
                        std::vector<GumboNode *> containers;
                        find_nodes(output->root, site.container_tag, site.container_class, containers);

                        std::cout << "  Found " << containers.size() << " job listings" << std::endl;

                        // Extract job details from each container
                        for (auto *container : containers)
                        {
                            json job = scrape_details(container, site, search_cfg);

                            // Only add valid jobs
                            if (!job.empty())
                            {
                                all_jobs.push_back(job);

                                // Print basic info about the job
                                std::cout << "  Scraped: "
                                          << job.value("title", "Unknown Title") << " at "
                                          << job.value("company", "Unknown Company") << " in "
                                          << job.value("location", "Unknown Location") << std::endl;

                                // Break if we've reached the maximum number of jobs
                                if (all_jobs.size() >= static_cast<size_t>(output_cfg.max_jobs))
                                {
                                    std::cout << "  Reached maximum job limit (" << output_cfg.max_jobs << ")" << std::endl;
                                    break;
                                }
                            }

                            // Add a small delay between jobs
                            std::this_thread::sleep_for(std::chrono::milliseconds(500 + (std::rand() % 1000)));
                        }

                        // Clean up Gumbo parser
                        gumbo_destroy_output(&kGumboDefaultOptions, output);

                        // Break if we've reached the maximum number of jobs
                        if (all_jobs.size() >= static_cast<size_t>(output_cfg.max_jobs))
                        {
                            break;
                        }

                        // Respect the site's delay between requests to avoid being blocked
                        std::this_thread::sleep_for(site.delay + std::chrono::milliseconds(std::rand() % 5000));
                    }
                }
            }
            catch (const std::exception &e)
            {
                std::cerr << "Error scraping " << site.name << ": " << e.what() << std::endl;
            }

            // Break if we've reached the maximum number of jobs across all sites
            if (all_jobs.size() >= static_cast<size_t>(output_cfg.max_jobs))
            {
                std::cout << "Reached overall maximum job limit (" << output_cfg.max_jobs << ")" << std::endl;
                break;
            }

            // Add a longer delay between different sites to avoid detection
            std::cout << "Adding delay between job sites..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(15 + (std::rand() % 15)));
        }

        // Generate timestamped filename for output
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << output_cfg.output_dir << "/jobs_"
           << std::put_time(std::localtime(&now_time_t), "%Y%m%d_%H%M%S") << ".json";
        std::string output_path = ss.str();

        // Deduplicate jobs before saving
        std::vector<json> unique_jobs = deduplicate_jobs(all_jobs);
        std::cout << "Filtered " << all_jobs.size() << " jobs down to " << unique_jobs.size()
                  << " unique jobs" << std::endl;

        // Save to JSON file
        if (output_cfg.json_output && !unique_jobs.empty())
        {
            try
            {
                save_to_json(unique_jobs, output_path);
                std::cout << "Saved " << unique_jobs.size() << " jobs to " << output_path << std::endl;
            }
            catch (const std::exception &e)
            {
                std::cerr << "Error saving to JSON: " << e.what() << std::endl;
            }
        }
        else
        {
            if (unique_jobs.empty())
            {
                std::cout << "No jobs to save!" << std::endl;
            }
            else
            {
                std::cout << "JSON output is disabled" << std::endl;
            }
        }

        // Also save as CSV for easier viewing
        std::string csv_path = output_path.substr(0, output_path.length() - 5) + ".csv";
        try
        {
            save_to_csv(unique_jobs, csv_path);
            std::cout << "Saved " << unique_jobs.size() << " jobs to " << csv_path << std::endl;
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error saving to CSV: " << e.what() << std::endl;
        }

        // If this is a one-time run (interval is zero), break the loop
        if (output_cfg.scrape_interval.count() <= 0)
        {
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