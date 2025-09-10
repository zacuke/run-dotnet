#include "https_download.h"
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/system/error_code.hpp>
#include <limits>
#include <stdexcept>
#include <cstdlib>
#include <iostream>
namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

void https_download(const std::string& host, const std::string& target, const fs::path& outFile)
{
    net::io_context ioc;
    ssl::context ctx{ssl::context::tls_client};
    ctx.set_default_verify_paths();

    tcp::resolver resolver(ioc);
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

    if(!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
        throw beast::system_error(
            beast::error_code(static_cast<int>(::ERR_get_error()),
                              net::error::get_ssl_category()),
            "Failed to set SNI hostname");

    auto const results = resolver.resolve(host, "443");
    beast::get_lowest_layer(stream).connect(results);

 
    stream.set_verify_mode(ssl::verify_peer);
 
    stream.handshake(ssl::stream_base::client);

    http::request<http::string_body> req{http::verb::get, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "boost-beast-client");
    http::write(stream, req);

    beast::flat_buffer buffer;
    beast::error_code ec;

    http::response_parser<http::file_body> parser;
    parser.get().body().open(outFile.string().c_str(), beast::file_mode::write, ec);
    if (ec) throw beast::system_error{ec};
    http::read(stream, buffer, parser, ec);
    if (ec && ec != http::error::end_of_stream)
        throw beast::system_error{ec};

    stream.shutdown(ec);
    if (ec == net::error::eof || ec == ssl::error::stream_truncated)
        ec = {};
    if (ec) throw beast::system_error{ec};
}
static void log(const std::string& msg) {
    std::cerr << msg << std::endl;
    std::cerr.flush();
}
std::string https_get_string(const std::string& host, const std::string& target)
{
    net::io_context ioc;
    ssl::context ctx{ssl::context::tls_client};
    ctx.set_default_verify_paths();

    tcp::resolver resolver(ioc);
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

    // Enable SNI
    if(!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
        throw std::runtime_error("Failed to set SNI hostname");

    auto const results = resolver.resolve(host, "443");
    auto& lowest = beast::get_lowest_layer(stream);
    lowest.connect(results);

    stream.set_verify_mode(ssl::verify_peer);
    stream.handshake(ssl::stream_base::client);

    http::request<http::string_body> req{http::verb::get, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "boost-beast-client");
    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::dynamic_body> res;
    http::read(stream, buffer, res);

    std::string body;
    for (auto const& b : res.body().data())
        body.append(static_cast<const char*>(b.data()), b.size());

    // 🔥 No TLS shutdown -- just close socket
    beast::error_code ec;
    lowest.socket().shutdown(tcp::socket::shutdown_both, ec);
    lowest.socket().close(ec);

    return body;
}