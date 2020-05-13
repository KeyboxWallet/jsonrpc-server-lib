//
// Copyright (c) 2016-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

//------------------------------------------------------------------------------
//
// Example: WebSocket server, asynchronous
//
//------------------------------------------------------------------------------
#ifndef JSON_RPC_SERVER_FACTORY_IMPLEMENTATION
#define JSON_RPC_SERVER_FACTORY_IMPLEMENTATION

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <stdlib.h>
#include "json-rpc-interface.hpp"

using tcp = boost::asio::ip::tcp;              // from <boost/asio/ip/tcp.hpp>
namespace websocket = boost::beast::websocket; // from <boost/beast/websocket.hpp>
namespace beast = boost::beast;
using json = nlohmann::json;
//------------------------------------------------------------------------------

// Report a failure
void fail(boost::system::error_code ec, char const *what)
{
    std::cerr << what << ": " << ec.message() << "\n";
}

// Echoes back all received WebSocket messages
class json_rpc_session : public std::enable_shared_from_this<json_rpc_session>, public generic_json_rpc_session
{
    websocket::stream<tcp::socket> ws_;
    boost::beast::flat_buffer in_buffer_;
    json_rpc_context_free_server *context_free_server;
    json_rpc_context_server *context_server;

  public:
    // Take ownership of the socket
    explicit json_rpc_session(
        tcp::socket socket,
        json_rpc_context_free_server *server)
        : ws_(std::move(socket))
    {
        context_free_server = server;
        context_server = NULL;
    }

    explicit json_rpc_session(
        tcp::socket socket,
        json_rpc_context_server *server)
        : ws_(std::move(socket))
    {
        context_server = server;
	    context_free_server = NULL;
    }

    // Start the asynchronous operation
    void
    run()
    {
        ws_.set_option(
            websocket::stream_base::timeout::suggested(
                beast::role_type::server));

        // Set a decorator to change the Server of the handshake
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res)
            {
                /*res.set(http::field::server,
                    std::string(BOOST_BEAST_VERSION_STRING) +
                        " websocket-server-async");*/
                 res.insert("Sec-WebSocket-Protocol", "json-rpc-server");
            }));
        // Accept the websocket handshake
        ws_.async_accept(
            beast::bind_front_handler(
                    &json_rpc_session::on_accept,
                    shared_from_this()
            ));
    }

    void
    on_accept(boost::system::error_code ec)
    {
        if (ec)
            return fail(ec, "accept");

        if( context_server ){
            context_server->session_added(this);
        }
        // Read a message
        do_read();
    }

    void
    do_read()
    {
        // Read a message into our buffer
        ws_.async_read(
            in_buffer_,
            beast::bind_front_handler(
                    &json_rpc_session::on_read,
                    shared_from_this()
            ));
    }

    void do_close()
    { // close connection because of other issue
        ws_.async_close(
            boost::beast::websocket::close_reason{
                boost::beast::websocket::bad_payload},
            std::bind(&json_rpc_session::on_close,
                      shared_from_this(),
                      std::placeholders::_1));
    }

    void
    on_read(
        boost::system::error_code ec,
        std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        // This indicates that the session was closed
        if (ec == websocket::error::closed)
        {
            std::cerr << "closed"
                      << "\n";
            return;
        }

        if (ec)
            fail(ec, "read");

        if (!ws_.got_text())
        {
            //
            return;
        }

        const boost::asio::mutable_buffer *c = boost::asio::buffer_sequence_begin(in_buffer_.data());
        nlohmann::detail::input_adapter i((char *)c->data(), c->size());
        json j;
        try
        {
            j = json::parse(std::move(i));
        }
        catch (json::exception &e)
        {
            in_buffer_.consume(in_buffer_.size());
            do_close();
            return;
        }
        in_buffer_.consume(in_buffer_.size());
        // Do another read
        do_read();
        std::string method;
        json params;
        json id;
        try
        {
            std::string version = j["jsonrpc"].get<std::string>();
            if (version != "2.0")
            {
                fail(ec, "version not match");
                do_close();
                return;
            }
            method = j["method"].get<std::string>();
            params = j["params"];
            id = j["id"];
        }
        catch (json::exception &e)
        {
            fail(ec, "parse");
            do_close();
            return;
        }
        if (context_free_server)
        {
            json result = context_free_server->call(method, params);
            json r;
            r["jsonrpc"] = "2.0";
            r["id"] = id;
            r["result"] = result;
            do_write_(r);
        }
        else if (context_server)
        {
            context_server->call(this, id, method, params);
        }
    }

    void on_close(
        boost::system::error_code ec)
    {
        if( context_server ){
            context_server->session_removed(this);
            context_server = NULL;
        }
    }

     virtual void do_reply(const json & id, const json & o) override
     {
         json r;
         r["id"] = id;
         r["jsonrpc"] = "2.0";
         r["result"] = o;
         do_write_(r);
     }
     virtual void do_notify( const std::string method_name, const json &o) override
     {
         json r;
         r["jsonrpc"] = "2.0";
         r["method"] = method_name;
         r["params"] = o;
         do_write(r);
     }



    void do_write(json &oj)
    {
        do_write_(oj);
    }

  private:
    void do_write_(
        json &oj)
    {
        boost::asio::streambuf *out_buffer = new boost::asio::streambuf();
        std::ostream o(out_buffer);
        o << oj;

        ws_.text(true);
        ws_.async_write(
            out_buffer->data(),
            beast::bind_front_handler(
                    &json_rpc_session::on_write,
                    shared_from_this(),
                    out_buffer));
    }

  public:
    void on_write(
        boost::asio::streambuf *out_buffer,
        boost::system::error_code ec,
        std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if (ec)
            return fail(ec, "write");

        out_buffer->consume(out_buffer->size());
        delete out_buffer;
    }

    ~json_rpc_session()
    {
        if( context_server ){
            context_server->session_removed(this);
            context_server = NULL;
        }
        std::cerr << "session end"
                  << "\n";
    }
};

//------------------------------------------------------------------------------

// Accepts incoming connections and launches the sessions
class json_rpc_server_factory : public std::enable_shared_from_this<json_rpc_server_factory>
{
    tcp::acceptor acceptor_;
    tcp::socket socket_;
    json_rpc_context_free_server *context_free_server;
    json_rpc_context_server *context_server;

  private:
    void init(
        boost::asio::io_context &ioc,
        tcp::endpoint endpoint)
    {
        boost::system::error_code ec;

        // Open the acceptor
        acceptor_.open(endpoint.protocol(), ec);
        if (ec)
        {
            fail(ec, "open");
            return;
        }

        // Allow address reuse
        acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
        if (ec)
        {
            fail(ec, "set_option");
            return;
        }

        // Bind to the server address
        acceptor_.bind(endpoint, ec);
        if (ec)
        {
            fail(ec, "bind");
            exit(1);
            return;
        }

        // Start listening for connections
        acceptor_.listen(
            boost::asio::socket_base::max_listen_connections, ec);
        if (ec)
        {
            fail(ec, "listen");
            return;
        }
    }

  public:
    explicit json_rpc_server_factory(
        boost::asio::io_context &ioc,
        tcp::endpoint endpoint,
        json_rpc_context_free_server *context_free_server) : acceptor_(ioc), socket_(ioc)
    {
        init(ioc, endpoint);
        this->context_free_server = context_free_server;
        this->context_server = NULL;
    }

    explicit json_rpc_server_factory(
        boost::asio::io_context &ioc,
        tcp::endpoint endpoint,
        json_rpc_context_server *context_server) : acceptor_(ioc), socket_(ioc)
    {
        init(ioc, endpoint);
        this->context_server = context_server;
        this->context_free_server = NULL;
    }

    // Start accepting incoming connections
    void
    run()
    {
        if (!acceptor_.is_open())
            return;
        do_accept();
    }

    void
    do_accept()
    {
        acceptor_.async_accept(
            socket_,
            std::bind(
                &json_rpc_server_factory::on_accept,
                shared_from_this(),
                std::placeholders::_1));
    }

    void
    on_accept(boost::system::error_code ec)
    {
        if (ec)
        {
            fail(ec, "accept");
            return;
        }
        else
        {
            // Create the session and run it
            std::shared_ptr<json_rpc_session> session;
            if (context_free_server)
            {
                session = std::make_shared<json_rpc_session>(
                    std::move(socket_),
                    context_free_server);
            }
            else
            {
                session = std::make_shared<json_rpc_session>(
                    std::move(socket_),
                    context_server);
            }
            session->run();
        }

        // Accept another connection
        do_accept();
    }
};

#endif
