#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/utils/FunctionTraits.h>
#include <memory>
#include <trantor/net/EventLoop.h>
#include <trantor/net/EventLoopThreadPool.h>
#include <trantor/net/InetAddress.h>
#include <trantor/net/TcpServer.h>
#include <trantor/utils/NonCopyable.h>

namespace spartoi
{

struct SpartanParseState
{
	drogon::HttpRequestPtr req;
	size_t content_length = 0;
	bool request_finished = 0;
};

class SpartanServer : public trantor::NonCopyable
{
public:
    SpartanServer(
            trantor::EventLoop* loop,
            const trantor::InetAddress& listenAddr);
    void start();
    void setIoThreadNum(size_t n);
    void setIoLoopThreadPool(const std::shared_ptr<trantor::EventLoopThreadPool>& pool);

protected:
    void sendResponseBack(const trantor::TcpConnectionPtr& conn, const drogon::HttpResponsePtr& resp);
    void onConnection(const trantor::TcpConnectionPtr &conn);
    void onMessage(const trantor::TcpConnectionPtr &conn, trantor::MsgBuffer *buf);
    trantor::EventLoop* loop_;
    trantor::TcpServer server_;
    std::atomic<int> roundRobbinIdx_{0};

	std::pair<drogon::HttpRequestPtr, size_t> parseHeader(const std::string& header);
	void processFinishedRequest(const drogon::HttpRequestPtr& req, trantor::TcpConnectionPtr conn);
};

}
