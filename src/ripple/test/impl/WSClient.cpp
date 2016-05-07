//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2016 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <BeastConfig.h>
#include <ripple/test/WSClient.h>
#include <ripple/test/jtx.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/to_string.h>
#include <ripple/protocol/JsonFields.h>
#include <ripple/server/Port.h>
#include <beast/core/placeholders.hpp>
#include <beast/core/streambuf.hpp>
#include <beast/websocket.hpp>
#include <condition_variable>

#include <ripple/beast/unit_test.h>

namespace ripple {
namespace test {

class WSClientImpl : public WSClient
{
    using error_code = boost::system::error_code;

    struct msg
    {
        Json::Value jv;

        explicit
        msg(Json::Value&& jv_)
            : jv(jv_)
        {
        }
    };

    static
    boost::asio::ip::tcp::endpoint
    getEndpoint(BasicConfig const& cfg, bool v2)
    {
        auto& log = std::cerr;
        ParsedPort common;
        parse_Port (common, cfg["server"], log);
        auto const ps = v2 ? "ws2" : "ws";
        for (auto const& name : cfg.section("server").values())
        {
            if (! cfg.exists(name))
                continue;
            ParsedPort pp;
            parse_Port(pp, cfg[name], log);
            if(pp.protocol.count(ps) == 0)
                continue;
            using boost::asio::ip::address_v4;
            if(*pp.ip == address_v4{0x00000000})
                *pp.ip = address_v4{0x7f000001};
            return { *pp.ip, *pp.port };
        }
        Throw<std::runtime_error>("Missing WebSocket port");
        return {}; // Silence compiler control paths return value warning
    }

    template <class ConstBuffers>
    static
    std::string
    buffer_string (ConstBuffers const& b)
    {
        using boost::asio::buffer;
        using boost::asio::buffer_size;
        std::string s;
        s.resize(buffer_size(b));
        buffer_copy(buffer(&s[0], s.size()), b);
        return s;
    }

    boost::asio::io_service ios_;
    boost::optional<
        boost::asio::io_service::work> work_;
    boost::asio::io_service::strand strand_;
    std::thread thread_;
    boost::asio::ip::tcp::socket stream_;
    beast::websocket::stream<boost::asio::ip::tcp::socket&> ws_;
    beast::websocket::opcode op_;
    beast::streambuf rb_;

    // synchronize destructor
    bool b0_ = false;
    std::mutex m0_;
    std::condition_variable cv0_;

    // synchronize message queue
    std::mutex m_;
    std::condition_variable cv_;
    std::list<std::shared_ptr<msg>> msgs_;

public:
    WSClientImpl(Config const& cfg, bool v2)
        : work_(ios_)
        , strand_(ios_)
        , thread_([&]{ ios_.run(); })
        , stream_(ios_)
        , ws_(stream_)
    {
        auto const ep = getEndpoint(cfg, v2);
        stream_.connect(ep);
        ws_.handshake(ep.address().to_string() +
            ":" + std::to_string(ep.port()), "/");
        ws_.async_read(op_, rb_,
            strand_.wrap(std::bind(&WSClientImpl::on_read_msg,
                this, beast::asio::placeholders::error)));
    }

    ~WSClientImpl() override
    {
        ws_.close({});
        stream_.close();
        work_ = boost::none;
        thread_.join();
    }

    Json::Value
    invoke(std::string const& cmd,
        Json::Value const& params) override
    {
        using boost::asio::buffer;
        using namespace std::chrono_literals;

        {
            Json::Value jp;
            if(params)
               jp = params;
            jp[jss::command] = cmd;
            auto const s = to_string(jp);
            ws_.write_frame(true, buffer(s));
        }

        auto jv = findMsg(5s,
            [&](Json::Value const& jv)
            {
                return jv[jss::type] == jss::response;
            });
        if (jv)
        {
            // Normalize JSON output
            jv->removeMember(jss::type);
            if ((*jv).isMember(jss::status) &&
                (*jv)[jss::status] == jss::error)
            {
                Json::Value ret;
                ret[jss::result] = *jv;
                if ((*jv).isMember(jss::error))
                    ret[jss::error] = (*jv)[jss::error];
                ret[jss::status] = jss::error;
                return ret;
            }

            if ((*jv).isMember(jss::status) &&
                (*jv).isMember(jss::result))
                    (*jv)[jss::result][jss::status] =
                        (*jv)[jss::status];
            return *jv;
        }
        return {};
    }

    boost::optional<Json::Value>
    getMsg(std::chrono::milliseconds const& timeout) override
    {
        std::shared_ptr<msg> m;
        {
            std::unique_lock<std::mutex> lock(m_);
            if(! cv_.wait_for(lock, timeout,
                    [&]{ return ! msgs_.empty(); }))
                return boost::none;
            m = std::move(msgs_.back());
            msgs_.pop_back();
        }
        return std::move(m->jv);
    }

    boost::optional<Json::Value>
    findMsg(std::chrono::milliseconds const& timeout,
        std::function<bool(Json::Value const&)> pred) override
    {
        std::shared_ptr<msg> m;
        {
            std::unique_lock<std::mutex> lock(m_);
            if(! cv_.wait_for(lock, timeout,
                [&]
                {
                    for (auto it = msgs_.begin();
                        it != msgs_.end(); ++it)
                    {
                        if (pred((*it)->jv))
                        {
                            m = std::move(*it);
                            msgs_.erase(it);
                            return true;
                        }
                    }
                    return false;
                }))
            {
                return boost::none;
            }
        }
        return std::move(m->jv);
    }

private:
    void
    on_read_msg(error_code const& ec)
    {
        if(ec)
            return;
        Json::Value jv;
        Json::Reader jr;
        jr.parse(buffer_string(rb_.data()), jv);
        rb_.consume(rb_.size());
        auto m = std::make_shared<msg>(
            std::move(jv));
        {
            std::lock_guard<std::mutex> lock(m_);
            msgs_.push_front(m);
            cv_.notify_all();
        }
        ws_.async_read(op_, rb_, strand_.wrap(
            std::bind(&WSClientImpl::on_read_msg,
                this, beast::asio::placeholders::error)));
    }

    // Called when the read op terminates
    void
    on_read_done()
    {
        std::lock_guard<std::mutex> lock(m0_);
        b0_ = true;
        cv0_.notify_all();
    }
};

std::unique_ptr<WSClient>
makeWSClient(Config const& cfg)
{
    return std::make_unique<WSClientImpl>(cfg, false);
}

std::unique_ptr<WSClient>
makeWS2Client(Config const& cfg)
{
    return std::make_unique<WSClientImpl>(cfg, true);
}

} // test
} // ripple
