#include "SpartanServer.hpp"
#include <drogon/HttpAppFramework.h>
#include <algorithm>
#include <charconv>
#include <memory>
#include <system_error>

using namespace drogon;
using namespace spartoi;
using namespace trantor;

namespace
{
constexpr size_t kMaxRequestHeaderBytes = 8 * 1024;
constexpr size_t kMaxRequestBodyBytes = 32 * 1024 * 1024;
constexpr size_t kMaxConnections = 256;
constexpr double kRequestDeadlineSeconds = 60;
}

SpartanServer::SpartanServer(EventLoop* loop, const InetAddress& listenAddr)
    : loop_(loop), server_(loop, listenAddr, "SpartanServer")
{
    LOG_DEBUG << "Creating srver on address " << listenAddr.toIpPort();

	server_.setConnectionCallback([this](const TcpConnectionPtr& conn) {onConnection(conn);});
	server_.setRecvMessageCallback([this](const TcpConnectionPtr& conn, MsgBuffer* buf){onMessage(conn, buf);});
	server_.kickoffIdleConnections(static_cast<size_t>(kRequestDeadlineSeconds));
}

SpartanServer::~SpartanServer() = default;

void SpartanServer::onConnection(const TcpConnectionPtr& conn)
{
	if(!conn->connected())
	{
		auto state = conn->getContext<SpartanParseState>();
		if(state && state->connection_permit)
		{
			state->connection_permit = false;
			activeConnections_.fetch_sub(1, std::memory_order_release);
		}
		return;
	}

	auto current = activeConnections_.load(std::memory_order_relaxed);
	while(current < kMaxConnections &&
	      !activeConnections_.compare_exchange_weak(current, current + 1,
	                                                std::memory_order_acquire,
	                                                std::memory_order_relaxed))
	{}
	if(current >= kMaxConnections)
	{
		conn->forceClose();
		return;
	}

	auto state = std::make_shared<SpartanParseState>();
	state->connection_permit = true;
	conn->setContext(state);
	const std::weak_ptr<TcpConnection> weakConn = conn;
	conn->getLoop()->runAfter(kRequestDeadlineSeconds, [weakConn]() {
		auto connection = weakConn.lock();
		if(!connection || !connection->connected())
			return;
		auto parseState = connection->getContext<SpartanParseState>();
		if(parseState && parseState->phase != SpartanParseState::Phase::Dispatched)
			connection->forceClose();
	});
}

void SpartanServer::start()
{
    server_.start();
}

void SpartanServer::stop()
{
	server_.stop();
}

void SpartanServer::setIoThreadNum(size_t n)
{
    server_.setIoLoopNum(n);
}

std::pair<HttpRequestPtr, size_t> SpartanServer::parseHeader(std::string_view header)
{
	const auto firstSpace = header.find(' ');
	const auto secondSpace = firstSpace == std::string_view::npos
	                       ? std::string_view::npos
	                       : header.find(' ', firstSpace + 1);
	if(firstSpace == std::string_view::npos || firstSpace == 0 ||
	   secondSpace == std::string_view::npos || secondSpace == firstSpace + 1 ||
	   header.find(' ', secondSpace + 1) != std::string_view::npos)
		throw std::invalid_argument("Invalid header");
	const auto host = header.substr(0, firstSpace);
	const auto target = header.substr(firstSpace + 1, secondSpace - firstSpace - 1);
	const auto lengthText = header.substr(secondSpace + 1);
	if(lengthText.empty())
		throw std::invalid_argument("Invalid content length");

	size_t contentLength = 0;
	const auto conversion = std::from_chars(lengthText.data(),
	                                        lengthText.data() + lengthText.size(),
	                                        contentLength);
	if(conversion.ec != std::errc{} || conversion.ptr != lengthText.data() + lengthText.size() ||
	   contentLength > kMaxRequestBodyBytes)
		throw std::invalid_argument("Invalid content length");

	auto req = HttpRequest::newHttpRequest();
	req->addHeader("host", std::string(host));
	req->setMethod(Get);
	req->addHeader("protocol", "spartan");

	const auto fragment = target.find('#');
	const auto withoutFragment = target.substr(0, fragment);
	const auto queryMark = withoutFragment.find('?');
	const auto path = withoutFragment.substr(0, queryMark);
	const auto query = queryMark == std::string_view::npos
	                 ? std::string_view{}
	                 : withoutFragment.substr(queryMark + 1);
	if((!path.empty() && path.front() != '/') ||
	   target.find('\r') != std::string_view::npos ||
	   target.find('\n') != std::string_view::npos)
		throw std::invalid_argument("Invalid path");

	req->setPath(std::string(path));
	req->setParameter("query", std::string(query));
	return {std::move(req), contentLength};
}

void SpartanServer::processFinishedRequest(const HttpRequestPtr& req, trantor::TcpConnectionPtr conn)
{
	auto state = conn->getContext<SpartanParseState>();
	if(!state)
		return;
	state->phase = SpartanParseState::Phase::Dispatched;
	const auto threadCount = app().getThreadNum();
	if(threadCount == 0)
	{
		rejectRequest(conn, "Server has no application I/O threads");
		return;
	}
	const auto idx = roundRobbinIdx_.fetch_add(1, std::memory_order_relaxed) % threadCount;
	// Drogon only accepts request from it's own event loops
	app().getIOLoop(idx)->runInLoop([req, conn=std::move(conn)]() mutable {
		app().forward(req, [req=std::move(req), conn=std::move(conn)](const HttpResponsePtr& resp){
			SpartanServer::sendResponseBack(conn, resp);
		});
	});
}

void SpartanServer::onMessage(const TcpConnectionPtr &conn, MsgBuffer *buf)
{
	auto context = conn->getContext<SpartanParseState>();
	if(!context)
	{
		buf->retrieveAll();
		conn->forceClose();
		return;
	}
	if(context->phase == SpartanParseState::Phase::Dispatched)
	{
		LOG_WARN << "Received data after complete Spartan request";
		buf->retrieveAll();
		conn->forceClose();
		return;
	}
	if(context->phase == SpartanParseState::Phase::AwaitingBody)
	{
		const auto remaining = context->content_length - context->body.size();
		if(buf->readableBytes() > remaining)
		{
			rejectRequest(conn, "Request body exceeds declared length");
			return;
		}
		context->body.append(buf->peek(), buf->readableBytes());
		buf->retrieveAll();
		if(context->body.size() != context->content_length)
			return;
		// MsgBuffer::retrieveAll() retains its allocation. Release that capacity
		// before copying the owned request parameter so a complete upload is not
		// kept in both buffers for the lifetime of the routed request.
		{
			MsgBuffer oldBuffer;
			buf->swap(oldBuffer);
		}
		context->req->setParameter("query", context->body);
		std::string{}.swap(context->body);
		processFinishedRequest(context->req, conn);
		return;
	}

	const auto scanStart = buf->peek() + std::min(context->header_scan_offset, buf->readableBytes());
	const char crlfBytes[] = {'\r', '\n'};
	const auto crlf = std::search(scanStart, buf->peek() + buf->readableBytes(),
	                              std::begin(crlfBytes), std::end(crlfBytes));
	if(crlf == buf->peek() + buf->readableBytes())
	{
		if(buf->readableBytes() > kMaxRequestHeaderBytes)
			rejectRequest(conn, "Request header is too long");
		else
			context->header_scan_offset = buf->readableBytes() == 0 ? 0 : buf->readableBytes() - 1;
		return;
	}
	const auto headerBytes = static_cast<size_t>(crlf - buf->peek());
	if(headerBytes + 2 > kMaxRequestHeaderBytes)
	{
		rejectRequest(conn, "Request header is too long");
		return;
	}
	const std::string_view header(buf->peek(), headerBytes);
	HttpRequestPtr req;
	size_t content_length;
	try {
		std::tie(req, content_length) = parseHeader(header);
	}
	catch(std::exception& e) {
		LOG_WARN << "Invalid Spartan request header";
		rejectRequest(conn, "Invalid request header");
		return;
	}

	buf->retrieve(headerBytes + 2);
	LOG_TRACE << "Spartan request received";
	context->req = req;
	context->content_length = content_length;
	if(content_length == 0) {
		if(buf->readableBytes() != 0)
		{
			rejectRequest(conn, "Unexpected data after request");
			return;
		}
		processFinishedRequest(req, conn);
	}
	else {
		context->phase = SpartanParseState::Phase::AwaitingBody;
		onMessage(conn, buf);
	}
}

void SpartanServer::rejectRequest(const TcpConnectionPtr &conn, const std::string &message)
{
	if(auto state = conn->getContext<SpartanParseState>())
		state->phase = SpartanParseState::Phase::Dispatched;
	auto resp = HttpResponse::newHttpResponse();
	resp->setStatusCode(k400BadRequest);
	resp->addHeader("meta", message);
	sendResponseBack(conn, resp);
}


void SpartanServer::setIoLoopThreadPool(const std::shared_ptr<EventLoopThreadPool>& pool)
{
    server_.setIoLoopThreadPool(pool);
}

void SpartanServer::sendResponseBack(const TcpConnectionPtr& conn, const HttpResponsePtr& resp)
{
    LOG_TRACE << "Sending response back";
    const int httpStatus = resp->statusCode();
    int status;
    if(httpStatus < 100) // HTTP status starts from 100. These are Spartan status
        status = httpStatus;
    else if(httpStatus/100 == 2) // HTTP 200 Ok -> Spartan 2 OK
        status = 2;
    else if(httpStatus == 404) // 404 (Not Found) -> Spartan 5 Server Error
        status = 5;
    else if(httpStatus == 400) // 400 (Bad Request) -> Spartan 4 Client Error
        status = 4;
    else if(httpStatus == 307 || httpStatus == 308) // 307/308 redirect -> Spartan 3 Redirect
        status = 3;
	else
		status = 5; // else -> Spartan 5 Server Error

    std::string respHeader;
    assert(status < 6 && status >= 2 || status >= 10 && status < 100);

	// HACK: Gemini compatiblity hack: Send a custom redirection form
	if(status/10 == 1) {
		const auto state = conn->getContext<SpartanParseState>();
		if(!state || !state->req)
		{
			conn->forceClose();
			return;
		}
		auto meta = state->req->getHeader("meta");
		std::string body = "=: " + state->req->path() + " " + (meta.empty() ? std::string("Input required") : meta);
		conn->send("2 text/gemini\r\n" + body);
		conn->shutdown();
		return;
	}

	if(status == 2)
    {
        auto ct = resp->contentTypeString();
        if(ct != "")
            respHeader = std::to_string(status) + " " + ct + "\r\n";
        else
            respHeader = std::to_string(status) + " application/octet-stream\r\n";
    }
	else if(status == 3)
	{
		respHeader = std::to_string(status) + " " + resp->getHeader("Location") + "\r\n";
	}
	else if(httpStatus == 404)
	{
		const auto state = conn->getContext<SpartanParseState>();
		if(!state || !state->req)
		{
			conn->forceClose();
			return;
		}
		respHeader = "4 Path " + state->req->path() + " Not Found\r\n";
	}
	else
	{
		auto meta = resp->getHeader("meta");
		if(meta.empty())
			meta = "Spartoi encountered an error. HTTP status code: " + std::to_string(httpStatus);
		meta.erase(std::remove(meta.begin(), meta.end(), '\r'), meta.end());
		meta.erase(std::remove(meta.begin(), meta.end(), '\n'), meta.end());
		respHeader = std::to_string(status) + " " + meta + "\r\n";
	}
    conn->send(respHeader);

    if(status == 2)
    {
        const std::string &sendfileName = resp->sendfileName();
        if (!sendfileName.empty())
        {
            const auto &range = resp->sendfileRange();
            conn->sendFile(sendfileName.c_str(), range.first, range.second);
        }
        else if(resp->body().size() != 0)
        {
            conn->send(std::string(resp->body()));
        }
    }
    conn->shutdown();
}
