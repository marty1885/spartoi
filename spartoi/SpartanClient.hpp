#pragma once

#include <drogon/HttpTypes.h>
#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <trantor/net/EventLoop.h>
#include <trantor/net/InetAddress.h>
#include <trantor/utils/Logger.h>
#include <trantor/net/callbacks.h>

#include <memory>
#include <optional>
#include <string>
#include <stdexcept>

#ifdef __cpp_impl_coroutine
#include <drogon/utils/coroutine.h>
#endif

// Forward declaration of heavy trantor classes
namespace trantor
{
class TcpClient;
class Resolver;
}

namespace spartoi
{

namespace internal
{

class SpartanClient : public std::enable_shared_from_this<SpartanClient>
{
public:
    SpartanClient(std::string url, trantor::EventLoop* loop, double timeout = 0, intmax_t maxBodySize = 0x2000000, double maxTransferDuration = 900);
    void fire();
    void setCallback(const drogon::HttpReqCallback& callback)
    {
        callback_ = callback;
    }
    void setCallback(drogon::HttpReqCallback&& callback)
    {
        callback_ = std::move(callback);
    }

    /**
     * @brief Set a set of allowed MIMEs for the response. If the response is 2 OK but the returned MIME is not in the set,
     *       the client immediately closes the connection and uses the header as is with empty body. Akin to the HEAD method
     *       when MIME is not in the set
     * 
     * @param mimes the set of allowed MIMEs.
     */
    void setMimes(const std::vector<std::string>& mimes)
    {
        downloadMimes_ = mimes;
    }

protected:
    void sendRequestInLoop();
    void onRecvMessage(const trantor::TcpConnectionPtr &connPtr,
                    trantor::MsgBuffer *msg);
    void haveResult(drogon::ReqResult result, const trantor::MsgBuffer* msg);

    // User specifable values
    trantor::EventLoop* loop_;
    double timeout_;
    drogon::HttpReqCallback callback_;
    std::string url_;
    intmax_t maxBodySize_;
    double maxTransferDuration_;

    // Internal state
    std::shared_ptr<trantor::TcpClient> client_;
    std::string host_;
	std::string path_;
    short port_;
    trantor::InetAddress peerAddress_;
    bool headerReceived_ = false;
    int responseStatus_ = 0;
    std::string resoneseMeta_;
    std::shared_ptr<trantor::Resolver> resolver_;
    trantor::TimerId timeoutTimerId_;
    std::vector<std::string> downloadMimes_;
    trantor::TimerId transferTimerId_;
    bool callbackCalled_ = false;
    size_t headerScanOffset_ = 0;
};

}

void sendRequest(const std::string& url, const drogon::HttpReqCallback& callback, double timeout = 10
	, trantor::EventLoop* loop=drogon::app().getLoop(), intmax_t maxBodySize = 0x2000000, const std::vector<std::string>& mimes = {}
	, double maxTransferDuration=900);

#ifdef __cpp_impl_coroutine
namespace internal
{

struct [[nodiscard]] SpartanRespAwaiter
{
	SpartanRespAwaiter(std::string url, trantor::EventLoop* loop, double timeout = 10, intmax_t maxBodySize = 0x2000000, const std::vector<std::string>& mimes = {}
		, double maxTransferDuration=900)
		: url_(url), loop_(loop), timeout_(timeout), maxBodySize_(maxBodySize), mimes_(mimes), maxTransferDuration_(maxTransferDuration)
	{
	}

	SpartanRespAwaiter(const SpartanRespAwaiter&) = delete;
	SpartanRespAwaiter& operator=(const SpartanRespAwaiter&) = delete;
	SpartanRespAwaiter(SpartanRespAwaiter&&) = default;
	SpartanRespAwaiter& operator=(SpartanRespAwaiter&&) = default;

	bool await_ready() const noexcept
	{
		return false;
	}

	void await_suspend(std::coroutine_handle<> handle)
	{
		using namespace drogon;
		state_->handle = handle;
		std::weak_ptr<State> weakState = state_;
		sendRequest(url_, [weakState](ReqResult res, HttpResponsePtr resp){
			auto state = weakState.lock();
			if(!state)
				return;
			if (res == ReqResult::Ok)
				state->response = std::move(resp);
			else
			{
                std::string reason;
                if (res == ReqResult::BadResponse)
                    reason = "BadResponse";
                else if (res == ReqResult::NetworkFailure)
                    reason = "NetworkFailure";
                else if (res == ReqResult::BadServerAddress)
                    reason = "BadServerAddress";
                else if (res == ReqResult::Timeout)
                    reason = "Timeout";
                else if(res == ReqResult::HandshakeError)
                    reason = "HandshakeError";
                else if(res == ReqResult::InvalidCertificate)
                    reason = "InvalidCertificate";
				state->exception = std::make_exception_ptr(std::runtime_error(reason));
			}
			state->handle.resume();
		}, timeout_, loop_, maxBodySize_, mimes_, maxTransferDuration_);
	}

	drogon::HttpResponsePtr await_resume()
	{
		if(state_->exception)
			std::rethrow_exception(state_->exception);
		return std::move(*state_->response);
	}

private:
	struct State
	{
		std::optional<drogon::HttpResponsePtr> response;
		std::exception_ptr exception;
		std::coroutine_handle<> handle;
	};

	std::string url_;
    trantor::EventLoop* loop_;
    double timeout_;
    intmax_t maxBodySize_;
	std::vector<std::string> mimes_;
	double maxTransferDuration_;
	std::shared_ptr<State> state_ = std::make_shared<State>();
};
}

inline internal::SpartanRespAwaiter sendRequestCoro(const std::string& url, double timeout = 10
	, trantor::EventLoop* loop=drogon::app().getLoop(), intmax_t maxBodySize = 0x2000000, const std::vector<std::string>& mimes = {}
	, double maxTransferDuration = 900)
{
    return internal::SpartanRespAwaiter(url, loop, timeout, maxBodySize, mimes, maxTransferDuration);
}

#endif


}
