#include "https_download.h"

#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/system/error_code.hpp>

#include <archive.h>
#include <archive_entry.h>   // For sanity check
#include <zlib.h>            // For quick gzip magic check

#include <cstdlib>
#include <iostream>
#include <string>
#include <fstream>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace ssl   = net::ssl;
using tcp       = net::ip::tcp;

// -------------------------------------------------------------------
// Split https://host/path into host & path
// -------------------------------------------------------------------
static void split_url(const std::string& url, std::string& host, std::string& target) {
    if (url.rfind("https://", 0) == 0) {
        auto noScheme = url.substr(8);
        auto slashPos = noScheme.find('/');
        host = noScheme.substr(0, slashPos);
        target = (slashPos == std::string::npos) ? "/" : noScheme.substr(slashPos);
    } else {
        throw std::runtime_error("Only https:// URLs supported: " + url);
    }
}

// -------------------------------------------------------------------
// Quick gzip sanity check: reads first few bytes
// -------------------------------------------------------------------
static bool is_gzip_file(const fs::path& filePath) {
    std::ifstream f(filePath, std::ios::binary);
    if (!f.good()) return false;

    uint8_t magic[2];
    f.read(reinterpret_cast<char*>(magic), 2);
    if (f.gcount() < 2) return false;

    // gzip signature is 0x1F 0x8B
    return (magic[0] == 0x1F && magic[1] == 0x8B);
}

// -------------------------------------------------------------------
// Download with redirect following, and gzip check
// -------------------------------------------------------------------
void https_download(const std::string& hostInit,
                    const std::string& targetInit,
                    const fs::path& outFile)
{
    std::string host   = hostInit;
    std::string target = targetInit;
    int maxRedirects   = 5;

    for (int redirects = 0; redirects <= maxRedirects; ++redirects) {
        net::io_context ioc;
        ssl::context ctx{ssl::context::tls_client};
        ctx.set_default_verify_paths();

        tcp::resolver resolver{ioc};
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

        if(!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
            throw beast::system_error(
                beast::error_code(static_cast<int>(::ERR_get_error()),
                                  net::error::get_ssl_category()),
                "Failed to set SNI");

        auto const results = resolver.resolve(host, "443");
        beast::get_lowest_layer(stream).connect(results);

        stream.set_verify_mode(ssl::verify_peer);
        stream.handshake(ssl::stream_base::client);

        http::request<http::empty_body> req{http::verb::get, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "run-dotnet-bootstrapper");

        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response_parser<http::file_body> parser;
        parser.body_limit((std::numeric_limits<std::uint64_t>::max)()); // Unlimited
        
        beast::error_code ec;
        parser.get().body().open(outFile.string().c_str(),
                                 beast::file_mode::write, ec);
        if (ec) throw beast::system_error{ec};

        http::read(stream, buffer, parser, ec);
        if (ec && ec != http::error::end_of_stream)
            throw beast::system_error{ec};

        // Check for redirects
        auto status = parser.get().result();
        if (status >= http::status::moved_permanently && status < http::status::bad_request) {
            auto loc = parser.get().base()["Location"];
            if (!loc.empty()) {
                std::string locStr(loc);
                std::cerr << "Redirect to: " << locStr << "\n";
                split_url(locStr, host, target);
                continue; // retry
            }
        }

        if (status != http::status::ok) {
            throw std::runtime_error("Download failed, HTTP " +
                                     std::to_string(static_cast<int>(status)));
        }

        // --- validate gzip
        if (!is_gzip_file(outFile)) {
            throw std::runtime_error("Downloaded file is not a valid gzip archive: " +
                                     outFile.string());
        }

        std::cerr << "Saved archive to " << outFile << " (gzip verified)\n";
        return;
    }

    throw std::runtime_error("Too many redirects");
}

// -------------------------------------------------------------------
// https_get_string (unchanged)
// -------------------------------------------------------------------
std::string https_get_string(const std::string& host, const std::string& target)
{
    net::io_context ioc;
    ssl::context ctx{ssl::context::tls_client};
    ctx.set_default_verify_paths();

    tcp::resolver resolver(ioc);
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

    if(!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
        throw std::runtime_error("Failed to set SNI hostname");

    auto const results = resolver.resolve(host, "443");
    auto& lowest = beast::get_lowest_layer(stream);
    lowest.connect(results);

    stream.set_verify_mode(ssl::verify_peer);
    stream.handshake(ssl::stream_base::client);

    http::request<http::string_body> req{http::verb::get, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "run-dotnet-bootstrapper");

    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::dynamic_body> res;
    
    // Set a large body limit before reading
    http::parser<false, http::dynamic_body> parser;
    parser.body_limit((std::numeric_limits<std::uint64_t>::max)()); // Unlimited or very large
    
    http::read(stream, buffer, parser);
    res = parser.release();

    std::string body;
    for (auto const& b : res.body().data())
        body.append(static_cast<const char*>(b.data()), b.size());

    beast::error_code ec;
    lowest.socket().shutdown(tcp::socket::shutdown_both, ec);
    lowest.socket().close(ec);

    return body;
}