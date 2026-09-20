#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/utils/FunctionTraits.h>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <trantor/net/EventLoop.h>
#include <trantor/net/EventLoopThreadPool.h>
#include <trantor/net/InetAddress.h>
#include <trantor/net/TcpServer.h>
#include <trantor/utils/NonCopyable.h>

namespace spartoi
{

struct SpartanParseState
{
	enum class Phase
	{
		AwaitingHeader,
		AwaitingBody,
		Dispatched
	};

	drogon::HttpRequestPtr req;
	size_t content_length = 0;
	std::string body;
	size_t header_scan_offset = 0;
	Phase phase = Phase::AwaitingHeader;
	bool connection_permit = false;
};

class SpartanServer : public trantor::NonCopyable
{
public:
    SpartanServer(
		    trantor::EventLoop* loop,
		    const trantor::InetAddress& listenAddr);
	~SpartanServer();
	void start();
	void stop();
    void setIoThreadNum(size_t n);
    void setIoLoopThreadPool(const std::shared_ptr<trantor::EventLoopThreadPool>& pool);

protected:
	static void sendResponseBack(const trantor::TcpConnectionPtr& conn, const drogon::HttpResponsePtr& resp);
	void onConnection(const trantor::TcpConnectionPtr &conn);
	void onMessage(const trantor::TcpConnectionPtr &conn, trantor::MsgBuffer *buf);
	void rejectRequest(const trantor::TcpConnectionPtr &conn, const std::string &message);
	trantor::EventLoop* loop_;
	trantor::TcpServer server_;
	std::atomic_uint32_t roundRobbinIdx_{0};
	std::atomic_size_t activeConnections_{0};

	std::pair<drogon::HttpRequestPtr, size_t> parseHeader(std::string_view header);
	void processFinishedRequest(const drogon::HttpRequestPtr& req, trantor::TcpConnectionPtr conn);
};

}
