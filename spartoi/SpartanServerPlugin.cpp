#include "SpartanServerPlgin.hpp"
#include "SpartanServer.hpp"
#include <drogon/HttpAppFramework.h>
#include <drogon/utils/Utilities.h>
#include <json/value.h>
#include <memory>
#include <string>
#include <trantor/net/EventLoopThreadPool.h>

using namespace spartoi;
using namespace drogon;
using namespace trantor;

void SpartanServerPlugin::initAndStart(const Json::Value& config)
{
    int numThread = config.get("numThread", 1).asInt();
    if(numThread < 0)
    {
        LOG_FATAL << "numThread must be latger or equal to 1";
        exit(1);
    }

    pool_ = std::make_shared<trantor::EventLoopThreadPool>(numThread, "SpartanServerThreadPool");


    const auto& listeners = config["listeners"];
    if(listeners.isNull())
    {
        LOG_WARN << "Creating Spartan Server without litening to any IP";
    }
    else
    {
        for(const auto& listener : listeners)
        {
            auto ip = listener.get("ip", "").asString();
            short port = listener.get("port", 300).asInt();
            if(ip.empty())
            {
                LOG_FATAL << "Spartan Server IP not specsifed";
                exit(1);
            }

            bool isV6 = ip.find(":") != std::string::npos;
            InetAddress addr(ip, port, isV6);
            if(addr.isUnspecified())
            {
                LOG_FATAL << ip << " is not a valid IP address";
            }

            auto server = std::make_unique<SpartanServer>(app().getLoop(), addr);
            server->setIoLoopThreadPool(pool_);
            server->start();
            servers_.emplace_back(std::move(server));
        }
    }
}

void SpartanServerPlugin::shutdown()
{
}

