#include <iostream>
#include <string>
#include <curl/curl.h>

// A simple callback to capture the server's response
size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}
int main() {
    CURL *curl;
    CURLcode res;
    std::string readBuffer;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();

    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:9000/cern-test-bucket/");
        curl_easy_setopt(curl, CURLOPT_AWS_SIGV4, "aws:amz:us-east-1:s3");
        curl_easy_setopt(curl, CURLOPT_USERPWD, "minioadmin:minioadmin");
        
        // Timeout and Verbose for debugging
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

        std::cout << "Sending request to MinIO using CURLOPT_AWS_SIGV4..." << std::endl;
        
        res = curl_easy_perform(curl);

        if(res != CURLE_OK) {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        } else {
            long response_code;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            std::cout << "Success! HTTP Response Code: " << response_code << std::endl;
            
            if(response_code == 200) {
                std::cout << "Integration test passed! MinIO accepted the libcurl signature." << std::endl;
            } else {
                std::cout << "Server Response: " << readBuffer << std::endl;
            }
        }
        curl_easy_cleanup(curl);
    }
    curl_global_cleanup();
    return 0; // Required for standard C++ main()
}
