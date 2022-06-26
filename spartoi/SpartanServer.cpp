#include "SpartanServer.hpp"
#include <drogon/HttpAppFramework.h>
#include <memory>
#include <regex>

using namespace drogon;
using namespace spartoi;
using namespace trantor;

SpartanServer::SpartanServer(EventLoop* loop, const InetAddress& listenAddr)
    : loop_(loop), server_(loop, listenAddr, "SpartanServer")
{
    LOG_DEBUG << "Creating srver on address " << listenAddr.toIpPort();

    server_.setConnectionCallback([this](const TcpConnectionPtr& conn) {onConnection(conn);});
    server_.setRecvMessageCallback([this](const TcpConnectionPtr& conn, MsgBuffer* buf){onMessage(conn, buf);});
}

void SpartanServer::onConnection(const TcpConnectionPtr& conn)
{}

void SpartanServer::start()
{
    server_.start();
}

void SpartanServer::setIoThreadNum(size_t n)
{
    server_.setIoLoopNum(n);
}

std::pair<HttpRequestPtr, size_t> SpartanServer::parseHeader(const std::string& header)
{
	auto parts = drogon::utils::splitString(header, " ");
	if(parts.size() != 3)
		throw std::invalid_argument("Invalid header");

	auto req = HttpRequest::newHttpRequest();
	req->addHeader("host", parts[0]);
	req->setMethod(Get);
	req->addHeader("protocol", "spartan");

	static const std::regex re(R"((\/$|$|\/[^?]*)(?:\?([^#]*))?(?:#(.*))?)");
	std::smatch match;
	if(!std::regex_match(parts[1], match, re))
		throw std::invalid_argument("Invalid path");
	
	std::string path = match[1];
	std::string query = match[2];
	// std::string fragment = match[3];

	req->setPath(path);
	req->setParameter("query", query);
	return {req, std::stoull(parts[2])};
}

void SpartanServer::processFinishedRequest(const HttpRequestPtr& req, trantor::TcpConnectionPtr conn)
{
	int idx = roundRobbinIdx_++;
    if(idx > 0x7ffff) // random large number
    {
        // XXX: Properbally data race. But it happens rare enough
        roundRobbinIdx_ = 0;
    }
    idx = idx % app().getThreadNum();
    // Drogon only accepts request from it's own event loops
    app().getIOLoop(idx)->runInLoop([req=std::move(req), conn=std::move(conn), this](){
        app().forward(req, [req=std::move(req), conn=std::move(conn), this](const HttpResponsePtr& resp){
            sendResponseBack(conn, resp);
        });
    });
}

void SpartanServer::onMessage(const TcpConnectionPtr &conn, MsgBuffer *buf)
{
	auto context = conn->getContext<SpartanParseState>();
    if(context != nullptr) {
        auto& [req, content_length, finished] = *context;
		if(finished) {
			LOG_WARN << "Received more data than expected";
			return;
		}
		if(buf->readableBytes() >= content_length) {
			req->setBody(std::string(buf->peek(), content_length));
			finished = true;
			processFinishedRequest(req, conn);
		}
        return;
    }

	auto crlf = buf->findCRLF();
	if(crlf == nullptr)
		return;
	std::string header(buf->peek(), crlf);
	HttpRequestPtr req;
	size_t content_length;
	try {
		std::tie(req, content_length) = parseHeader(header);
	}
	catch(std::exception& e) {
		LOG_WARN << "Invalid header: " << header;
		auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k500InternalServerError);
        resp->addHeader("meta", "Invalid request header");
        sendResponseBack(conn, resp);
		return;
	}

	buf->retrieve(std::distance(buf->peek(), crlf + 2));
	LOG_TRACE << "Spartan request recived. Header: " << header;

	SpartanParseState state;
	state.req = req;
	state.content_length = content_length;
	if(content_length == 0) {
		state.request_finished = true;
		conn->setContext(std::make_shared<SpartanParseState>(state));
		processFinishedRequest(req, conn);
	}
	else {
		if(buf->readableBytes() >= content_length) {
			req->setBody(std::string(buf->peek(), content_length));
			state.request_finished = true;
			conn->setContext(std::make_shared<SpartanParseState>(state));
			processFinishedRequest(req, conn);
		}
		else
			conn->setContext(std::make_shared<SpartanParseState>(state));
	}
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
		const auto& [req, _, __] = *conn->getContext<SpartanParseState>();
		auto meta = req->getHeader("meta");
		std::string body = "=: " + req->path() + " " + (meta.empty() ? std::string("Input required") : meta);
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
		const auto& [req, _, __] = *conn->getContext<SpartanParseState>();
		respHeader = "4 Path " + req->path() + " Not Found\r\n";
	}
	else
	{
		respHeader = "Spartoi encountered an error. HTTP status code: " + std::to_string(httpStatus) + "\r\n";
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
