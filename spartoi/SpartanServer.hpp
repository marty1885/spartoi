#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/utils/FunctionTraits.h>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <trantor/net/EventLoop.h>
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
		AwaitingHttpResponse,
		SendingHttpResponse,
		Dispatched
	};

	drogon::HttpRequestPtr req;
	size_t content_length = 0;
	std::string body;
	size_t header_scan_offset = 0;
	Phase phase = Phase::AwaitingHeader;
	bool connection_permit = false;
	std::string http_peer_ip;
	bool http_head_request = false;
	size_t http_input_bytes = 0;
	std::string http_body;
	size_t http_body_offset = 0;
	size_t http_send_step = 0;
};

struct SpartanConnectionLimits
{
	std::atomic_size_t active_connections{0};
	std::mutex http_mutex;
	size_t active_http_responses = 0;
	std::unordered_map<std::string, size_t> active_http_by_peer;
};

class SpartanServer : public trantor::NonCopyable
{
public:
    SpartanServer(
		    trantor::EventLoop* loop,
		    const trantor::InetAddress& listenAddr,
		    std::shared_ptr<SpartanConnectionLimits> limits = {});
	~SpartanServer();
	void start();
	void stop();

protected:
	static void sendResponseBack(const trantor::TcpConnectionPtr& conn, const drogon::HttpResponsePtr& resp);
	void onConnection(const trantor::TcpConnectionPtr &conn);
	void onMessage(const trantor::TcpConnectionPtr &conn, trantor::MsgBuffer *buf);
	void rejectRequest(const trantor::TcpConnectionPtr &conn, const std::string &message);
	trantor::TcpServer server_;
	bool started_ = false;
	bool stopped_ = false;
	std::atomic_uint32_t roundRobbinIdx_{0};
	std::shared_ptr<SpartanConnectionLimits> limits_;

	std::pair<drogon::HttpRequestPtr, size_t> parseHeader(std::string_view header);
	void processFinishedRequest(const drogon::HttpRequestPtr& req, trantor::TcpConnectionPtr conn);
	void processAccidentalHttpRequest(drogon::HttpRequestPtr req,
	                                  const trantor::TcpConnectionPtr& conn);
	static void sendAccidentalHttpResponse(const std::weak_ptr<trantor::TcpConnection>& conn,
	                                       const drogon::HttpResponsePtr& resp);
	static void sendAccidentalHttpBody(const std::weak_ptr<trantor::TcpConnection>& conn);
};

}
