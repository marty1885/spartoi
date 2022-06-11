#include "SpartanClient.hpp"
#include <trantor/net/TcpClient.h>
#include <trantor/net/Resolver.h>
#include <trantor/utils/MsgBuffer.h>

#include <regex>
#include <string>
#include <sstream>
#include <algorithm>

using namespace drogon;

static bool isIPString(const std::string& str)
{
    bool isIpV6 = str.find(":") != std::string::npos;
    return !trantor::InetAddress(str, 0, isIpV6).isUnspecified();
}

static ContentType parseContentType(const string_view &contentType)
{
    static const std::unordered_map<string_view, ContentType> map_{
        {"text/html", CT_TEXT_HTML},
        {"application/x-www-form-urlencoded", CT_APPLICATION_X_FORM},
        {"application/xml", CT_APPLICATION_XML},
        {"application/json", CT_APPLICATION_JSON},
        {"application/x-javascript", CT_APPLICATION_X_JAVASCRIPT},
        {"text/css", CT_TEXT_CSS},
        {"text/xml", CT_TEXT_XML},
        {"text/xsl", CT_TEXT_XSL},
        {"application/octet-stream", CT_APPLICATION_OCTET_STREAM},
        {"image/svg+xml", CT_IMAGE_SVG_XML},
        {"application/x-font-truetype", CT_APPLICATION_X_FONT_TRUETYPE},
        {"application/x-font-opentype", CT_APPLICATION_X_FONT_OPENTYPE},
        {"application/font-woff", CT_APPLICATION_FONT_WOFF},
        {"application/font-woff2", CT_APPLICATION_FONT_WOFF2},
        {"application/vnd.ms-fontobject", CT_APPLICATION_VND_MS_FONTOBJ},
        {"application/pdf", CT_APPLICATION_PDF},
        {"image/png", CT_IMAGE_PNG},
        {"image/webp", CT_IMAGE_WEBP},
        {"image/avif", CT_IMAGE_AVIF},
        {"image/jpeg", CT_IMAGE_JPG},
        {"image/gif", CT_IMAGE_GIF},
        {"image/x-icon", CT_IMAGE_XICON},
        {"image/bmp", CT_IMAGE_BMP},
        {"image/icns", CT_IMAGE_ICNS},
        {"application/wasm", CT_APPLICATION_WASM},
        {"text/plain", CT_TEXT_PLAIN},
        {"multipart/form-data", CT_MULTIPART_FORM_DATA}};
    auto iter = map_.find(contentType);
    if (iter == map_.end())
        return CT_CUSTOM;
    return iter->second;
}



namespace spartoi
{
namespace internal
{

SpartanClient::SpartanClient(std::string url, trantor::EventLoop* loop, double timeout, intmax_t maxBodySize, double maxTransferDuration)
    : loop_(loop), timeout_(timeout), maxBodySize_(maxBodySize), maxTransferDuration_(maxTransferDuration)
{
    static const std::regex re(R"(([a-z]+):\/\/([^\/:]+)(?:\:([0-9]+))?($|\/.*))");
    std::smatch match;
    if(!std::regex_match(url, match, re))
        throw std::invalid_argument(url + " is no a valid url");

    std::string protocol = match[1];
    host_ = match[2];
    std::string port = match[3];
    std::string path = match[4];

    if(protocol != "spartan")
        throw std::invalid_argument("Must be a spartan URL");
    port_ = 300;
    if(port.empty() == false)
    {
        int portNum = std::stoi(port);
        if(portNum >= 65536 || portNum <= 0)
        {
            LOG_ERROR << port << "is not a valid port number";
        }
        port_ = portNum;
    }

    if(path.empty() && url.back() != '/') {
		url_ = url + "/";
		path_ = "/";
	}
    else {
		url_ = url;
		path_ = path;
	}
}

void SpartanClient::fire()
{
    if(isIPString(host_))
    {
        bool isIpV6 = host_.find(":") != std::string::npos;
        peerAddress_ = trantor::InetAddress(host_, port_, isIpV6);
        sendRequestInLoop();
        return;
    }
    resolver_ = trantor::Resolver::newResolver(loop_, 10);
    resolver_->resolve(host_, [thisPtr=shared_from_this()](const trantor::InetAddress &addr){
        if(addr.ipNetEndian() == 0)
        {
            thisPtr->haveResult(ReqResult::BadServerAddress, nullptr);
            return;
        }
        thisPtr->peerAddress_ = trantor::InetAddress(addr.toIp(), thisPtr->port_, addr.isIpV6());
        thisPtr->sendRequestInLoop();
    });
}

void SpartanClient::haveResult(drogon::ReqResult result, const trantor::MsgBuffer* msg)
{
    loop_->assertInLoopThread();
    if(callbackCalled_ == true)
        return;
    callbackCalled_ = true;

    if(timeout_ > 0)
        loop_->invalidateTimer(timeoutTimerId_);
    if(maxTransferDuration_ > 0)
        loop_->invalidateTimer(transferTimerId_);
    if(result != ReqResult::Ok)
    {
        client_ = nullptr;
        callback_(result, nullptr);
        return;
    }
    if(!headerReceived_)
    {
        client_ = nullptr;
        callback_(ReqResult::BadResponse, nullptr);
        return;
    }

    // check ok. now we can get the body
    auto resp = HttpResponse::newHttpResponse();
    resp->setBody(std::string(msg->peek(), msg->peek()+msg->readableBytes()));
    resp->addHeader("meta", resoneseMeta_);
    resp->addHeader("spartan-status", std::to_string(responseStatus_));
    int httpStatus;
    if(responseStatus_ == 2)
        httpStatus = 200;
    else if(responseStatus_ == 3)
        httpStatus = 301;
    else if(responseStatus_ == 4)
        httpStatus = 400;
    else if(responseStatus_ == 5)
        httpStatus = 500;
    else {
        LOG_WARN << "Invalid Spartan response: " << responseStatus_;
        httpStatus = responseStatus_;
    }
    resp->setStatusCode((HttpStatusCode)httpStatus);
    if(responseStatus_ == 2)
    {
        auto end = resoneseMeta_.find(";");
        if(end == std::string::npos)
            end = resoneseMeta_.size();
        std::string_view ct(resoneseMeta_.c_str(), end);
        resp->setContentTypeCodeAndCustomString(parseContentType(ct), resoneseMeta_);
        resp->addHeader("content-type", resoneseMeta_);
    }
    else
        resp->setContentTypeCode(CT_NONE);
    // we need the client no more. Let's release this as soon as possible to save open file descriptors
    client_ = nullptr;
    callback_(ReqResult::Ok, resp);
}

void SpartanClient::sendRequestInLoop()
{
    // TODO: Validate certificate
    auto weakPtr = weak_from_this();
    client_ = std::make_shared<trantor::TcpClient>(loop_, peerAddress_, "SpartanClient");

    client_->setMessageCallback([weakPtr](const trantor::TcpConnectionPtr &connPtr,
              trantor::MsgBuffer *msg) {
        auto thisPtr = weakPtr.lock();
        if (thisPtr)
        {
            thisPtr->onRecvMessage(connPtr, msg);
        }
    });
    client_->setConnectionCallback([weakPtr, this](const trantor::TcpConnectionPtr &connPtr) {
        auto thisPtr = weakPtr.lock();
        if(!thisPtr)
            return;
        LOG_TRACE << "This is " << (void*)thisPtr.get();

        if(connPtr->connected())
        {
            LOG_TRACE << "Connected to server. Sending request. Host and path is : "
				<< thisPtr->host_ << " " << thisPtr->path_;; 
            connPtr->send(host_ + " " + path_ + " 0\r\n");
        }
        else
        {
            haveResult(ReqResult::Ok, connPtr->getRecvBuffer());
        }
    });

    client_->setConnectionErrorCallback([weakPtr]() {
        auto thisPtr = weakPtr.lock();
        if (!thisPtr)
            return;
        // can't connect to server
        thisPtr->haveResult(ReqResult::NetworkFailure, nullptr);
    });

    if(timeout_ > 0)
    {
        timeoutTimerId_ = loop_->runAfter(timeout_, [weakPtr](){
            auto thisPtr = weakPtr.lock();
            if(!thisPtr)
                return;
            thisPtr->haveResult(ReqResult::Timeout, nullptr);
        });
    }
    if(maxTransferDuration_ > 0)
    {
        transferTimerId_ = loop_->runAfter(maxTransferDuration_, [weakPtr](){
            auto thisPtr = weakPtr.lock();
            if(!thisPtr)
                return;
            thisPtr->haveResult(ReqResult::Timeout, nullptr);
        });
    }
    client_->connect();
}

void SpartanClient::onRecvMessage(const trantor::TcpConnectionPtr &connPtr,
              trantor::MsgBuffer *msg)
{
    if(timeout_ > 0)
        loop_->invalidateTimer(timeoutTimerId_);
    LOG_TRACE << "Got data from Spartan server";
    if(!headerReceived_)
    {
        const char* crlf = msg->findCRLF();
        if(crlf == nullptr)
        {
            return;
        }
        headerReceived_ = true;

        const string_view header(msg->peek(), std::distance(msg->peek(), crlf));
        LOG_TRACE << "Spartan header is: " << header;
        if(header.size() < 1 || (header.size() >= 2 && header[1] != ' '))
        {
            // bad response
            haveResult(ReqResult::BadResponse, nullptr);
            return;
        }

        responseStatus_ = std::stoi(std::string(header.begin(), header.begin()+1));
        if(header.size() >= 4)
            resoneseMeta_ = std::string(header.begin()+2, header.end());
        if(!downloadMimes_.empty() && responseStatus_ == 2)
        {
            std::string mime = resoneseMeta_.substr(0, resoneseMeta_.find_first_of("; ,"));
            if(std::find(downloadMimes_.begin(), downloadMimes_.end(), mime) == downloadMimes_.end()) {
                msg->retrieveAll();
                LOG_TRACE << "Ignoring file of MIME " << mime;
                connPtr->forceClose(); // this triggers the connection close handler which will call haveResult
                return;
            }
        }
        msg->read(std::distance(msg->peek(), crlf)+2);
    }
    if(maxBodySize_ > 0 && msg->readableBytes() > size_t(maxBodySize_))
    {
        LOG_DEBUG << "Recived more data than " << maxBodySize_ << " bites";
        // bad response
        haveResult(ReqResult::BadResponse, nullptr);

        return;
    }

    if(timeout_ > 0)
    {
        auto weakPtr = weak_from_this();
        timeoutTimerId_ = loop_->runAfter(timeout_, [weakPtr](){
            auto thisPtr = weakPtr.lock();
            if(!thisPtr)
                return;
           thisPtr->haveResult(ReqResult::Timeout, nullptr);
        });
    }
}


}

static std::map<int, std::shared_ptr<internal::SpartanClient>> holder;
static std::mutex holderMutex;
void sendRequest(const std::string& url, const HttpReqCallback& callback, double timeout
    , trantor::EventLoop* loop, intmax_t maxBodySize, const std::vector<std::string>& mimes
    , double maxTransferDuration)
{
    auto client = std::make_shared<internal::SpartanClient>(url, loop, timeout, maxBodySize, maxTransferDuration);
    int id;
    {
        std::lock_guard lock(holderMutex);
        for(id = std::abs(rand())+1; holder.find(id) != holder.end(); id = std::abs(rand())+1);
        holder[id] = client;
    }
    client->setCallback([callback, id, loop] (ReqResult result, const HttpResponsePtr& resp) mutable {
        callback(result, resp);

        std::lock_guard lock(holderMutex);
        auto it = holder.find(id);
        assert(it != holder.end());
        loop->queueInLoop([client = it->second]() {
            // client is destroyed here
        });
        holder.erase(it);
    });
    client->setMimes(mimes);
    client->fire();
}
}
