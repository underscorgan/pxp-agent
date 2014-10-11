#include "src/agent/agent_endpoint.h"
#include "src/agent/modules/echo.h"
#include "src/agent/modules/inventory.h"
#include "src/agent/modules/ping.h"
#include "src/agent/external_module.h"
#include "src/agent/schemas.h"
#include "src/agent/errors.h"
#include "src/common/log.h"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <mutex>

// TODO(ale): disable assert() once we're confident with the code...
// To disable assert()
// #define NDEBUG
#include <cassert>

LOG_DECLARE_NAMESPACE("agent.agent_endpoint");

namespace Cthun {
namespace Agent {

//
// Tokens
//

static const int DEFAULT_HEARTBEAT_PERIOD { 30 };  // [s]

//
// HeartbeatTask
//

HeartbeatTask::HeartbeatTask(Cthun::WebSocket::Connection::Ptr connection_ptr)
        : must_stop_ { false } {
    assert(connection_ptr != nullptr);
    connection_ptr_ = connection_ptr;
}

HeartbeatTask::~HeartbeatTask() {
    stop();
}

void HeartbeatTask::start() {
    LOG_INFO("starting the heartbeat task");
    heartbeat_thread_ = std::thread(&HeartbeatTask::heartbeatThread, this);
}

void HeartbeatTask::stop() {
    LOG_INFO("stopping the heartbeat task");
    must_stop_ = true;
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
}

void HeartbeatTask::heartbeatThread() {
    while (!must_stop_) {
        try {
            if (connection_ptr_->getState()
                    == Cthun::WebSocket::Connection_State_Values::open) {
                Cthun::WebSocket::CONNECTION_MANAGER.ping(connection_ptr_,
                                                          binary_payload_);
            } else {
                LOG_DEBUG("skipping ping; connection is not open");
            }
        } catch (Cthun::WebSocket::message_error& e) {
            LOG_ERROR(e.what());
        }
        sleep(DEFAULT_HEARTBEAT_PERIOD);
    }
}

//
// AgentEndpoint
//

AgentEndpoint::AgentEndpoint() {
    // declare internal modules
    modules_["echo"] = std::unique_ptr<Module>(new Modules::Echo);
    modules_["inventory"] = std::unique_ptr<Module>(new Modules::Inventory);
    modules_["ping"] = std::unique_ptr<Module>(new Modules::Ping);

    // load external modules
    boost::filesystem::path module_path { "modules" };
    boost::filesystem::directory_iterator end;

    for (auto file = boost::filesystem::directory_iterator(module_path);
            file != end; ++file) {
        if (!boost::filesystem::is_directory(file->status())) {
            LOG_INFO(file->path().string());

            try {
                ExternalModule* external =
                    new ExternalModule(file->path().string());
                modules_[external->module_name] =
                    std::shared_ptr<Module>(external);
            } catch (...) {
                LOG_ERROR("failed to load: %1%", file->path().string());
            }
        }
    }
}

AgentEndpoint::~AgentEndpoint() {
    if (connection_ptr_ != nullptr) {
        // reset callbacks to avoid breaking the WebSocket Endpoint
        // with invalid references

        LOG_INFO("resetting the WebSocket event callbacks");

        Cthun::WebSocket::Connection::Event_Callback onOpen_c =
            [](Cthun::WebSocket::Client_Type* client_ptr,
               Cthun::WebSocket::Connection::Ptr connection_ptr) {};

        Cthun::WebSocket::Connection::OnMessage_Callback onMessage_c =
            [](Cthun::WebSocket::Client_Type* client_ptr,
               Cthun::WebSocket::Connection::Ptr connection_ptr,
               std::string message) {};

        Cthun::WebSocket::Connection::Pong_Callback onPong_c =
            [](Cthun::WebSocket::Client_Type* client_ptr,
               Cthun::WebSocket::Connection::Ptr connection_ptr,
               std::string message) {};

        connection_ptr_->setOnOpenCallback(onOpen_c);
        connection_ptr_->setOnMessageCallback(onMessage_c);
        connection_ptr_->setOnPongCallback(onPong_c);
        connection_ptr_->setOnPongTimeoutCallback(onPong_c);

        // NB: the heartbeat task will be destructed by RIIA
    }
}

void AgentEndpoint::run(std::string module, std::string action) {
    list_modules();

    std::shared_ptr<Module> the_module = modules_[module];

    Json::Reader reader;
    Json::Value input;

    LOG_INFO("loading stdin");

    std::string command_line;
    std::getline(std::cin, command_line);

    if (!command_line.empty() && !reader.parse(command_line, input)) {
        LOG_ERROR("parse error: %1%", reader.getFormatedErrorMessages());
        return;
    }

    try {
        Json::Value output;
        the_module->validate_and_call_action(action, input, output);
        LOG_INFO(output.toStyledString());
    } catch (...) {
        LOG_ERROR("badness occured");
    }
}

void AgentEndpoint::connect_and_run(std::string url,
                                    std::string ca_crt_path,
                                    std::string client_crt_path,
                                    std::string client_key_path) {
    // Configure the secure WebSocket endpoint

    try {
        Cthun::WebSocket::CONNECTION_MANAGER.configureSecureEndpoint(
            ca_crt_path, client_crt_path, client_key_path);
    } catch (Cthun::WebSocket::endpoint_error& e) {
        LOG_WARNING("failed to configure the WebSocket endpoint: %1%", e.what());
        throw fatal_error("failed to configure the WebSocket endpoint");
    }

    // Configure the connection

    connection_ptr_ = Cthun::WebSocket::CONNECTION_MANAGER.createConnection(url);
    setConnectionCallbacks();

    // Connect and wait for open connection event

    try {
        Cthun::WebSocket::CONNECTION_MANAGER.open(connection_ptr_);
        connection_ptr_->waitForOpen();
    } catch (Cthun::WebSocket::connection_error& e) {
        LOG_WARNING("failed to connect; %1%", e.what());
        throw fatal_error { "failed to connect" };
    }

    // Start heartbeat task

    HeartbeatTask heartbeat_task { connection_ptr_ };
    heartbeat_task.start();

    // Keep monitoring the connection

    monitorConnectionState();
}

//
// AgentEndpoint - private
//

void AgentEndpoint::list_modules() {
    LOG_INFO("loaded modules:");
    for (auto module : modules_) {
        LOG_INFO("   %1%", module.first);
        for (auto action : module.second->actions) {
            LOG_INFO("       %1%", action.first);
        }
    }
}

void AgentEndpoint::send_login(Cthun::WebSocket::Client_Type* client_ptr) {
    Json::Value login {};
    login["id"] = 1;
    login["version"] = "1";
    login["expires"] = "2014-08-28T17:01:05Z";
    login["sender"] = "cth://localhost/agent";
    login["endpoints"] = Json::Value { Json::arrayValue };
    login["endpoints"][0] = "cth://server";
    login["hops"] = Json::Value { Json::arrayValue };
    login["data_schema"] = "http://puppetlabs.com/loginschema";
    login["data"]["type"] = "agent";


    LOG_INFO("login message:\n%1%", login.toStyledString());

    valijson::Schema message_schema = Schemas::network_message();
    std::vector<std::string> errors;

    if (!Schemas::validate(login, message_schema, errors)) {
        LOG_WARNING("validation failed");
        for (auto error : errors) {
            LOG_WARNING("    %1%", error);
        }
        // This is unexpected
        throw fatal_error { "invalid login message schema" };
    }

    auto handle = connection_ptr_->getConnectionHandle();
    try {
        client_ptr->send(handle, login.toStyledString(),
                         Cthun::WebSocket::Frame_Opcode_Values::text);
    }  catch(Cthun::WebSocket::message_error& e) {
        LOG_WARNING("failed to send: %1%", e.what());
        // Fatal; we can't login...
        throw fatal_error { "failed to send login message" };
    }
}

void AgentEndpoint::handle_message(Cthun::WebSocket::Client_Type* client_ptr,
                                   std::string message) {
    LOG_INFO("received message:\n%1%", message);

    Json::Value doc;
    Json::Reader reader;

    if (!reader.parse(message, doc)) {
        LOG_ERROR("json decode of message failed");
        return;
    }

    valijson::Schema message_schema = Schemas::network_message();
    std::vector<std::string> errors;

    if (!Schemas::validate(doc, message_schema, errors)) {
        LOG_ERROR("message schema validation failed");
        return;
    }

    if (std::string("http://puppetlabs.com/cncschema").compare(
            doc["data_schema"].asString()) != 0) {
        LOG_ERROR("message is not of cnc schema");
        return;
    }

    valijson::Schema data_schema { Schemas::cnc_data() };
    if (!Schemas::validate(doc["data"], data_schema, errors)) {
        // TODO(ale): refactor error logging
        LOG_ERROR("data schema validation failed");
        for (auto error : errors) {
            LOG_ERROR("    %1%", error);
        }
        return;
    }

    try {
        Json::Value output;
        auto module_name = doc["data"]["module"].asString();
        auto action_name = doc["data"]["action"].asString();

        if (modules_.find(module_name) != modules_.end()) {
            std::shared_ptr<Module> module = modules_[module_name];

            try {
                module->validate_and_call_action(action_name,
                                                 doc["data"]["params"],
                                                 output);
                LOG_DEBUG("%1% %2% output: %3%",
                          module_name, action_name, output.toStyledString());
            } catch (validation_error& e) {
                LOG_ERROR("failed to perform '%1% %2%': %3%",
                          module_name, action_name, e.what());
                Json::Value err_result;
                err_result["error"] = e.what();
                output = err_result;
            }
        } else {
            LOG_ERROR("invalid request: unknown module %1%", module_name)
            Json::Value err_result;
            err_result["error"] = "Unknown module: '" + module_name + "'";
            output = err_result;
        }

        Json::Value response {};
        response["id"] = 2;
        response["version"] = "1";
        response["expires"] = "2014-08-28T17:01:05Z";
        response["sender"] = "cth://localhost/agent";
        response["endpoints"] = Json::Value { Json::arrayValue };
        response["endpoints"][0] = doc["sender"];
        response["hops"] = Json::Value { Json::arrayValue };
        response["data_schema"] = "http://puppetlabs.com/cncresponseschema";
        response["data"]["response"] = output;

        auto response_txt = response.toStyledString();
        LOG_INFO("sending response of size %1%", response_txt.size());
        LOG_DEBUG("response:\n%1%", response_txt);

        auto handle = connection_ptr_->getConnectionHandle();
        client_ptr->send(handle,
                         response_txt,
                         Cthun::WebSocket::Frame_Opcode_Values::text);
    }  catch(Cthun::WebSocket::message_error& e) {
        LOG_ERROR("failed to send: %1%", e.what());
        // we don't want to throw anything here
    } catch (std::exception&  e) {
        LOG_ERROR("unexpected exception: %1%", e.what());
    } catch (...) {
        LOG_ERROR("badness occured");
    }
}

void AgentEndpoint::setConnectionCallbacks() {
    Cthun::WebSocket::Connection::Event_Callback onOpen_c =
        [this](Cthun::WebSocket::Client_Type* client_ptr,
               Cthun::WebSocket::Connection::Ptr connection_ptr_c) {
            assert(connection_ptr_ == connection_ptr_c);
            send_login(client_ptr);
        };

    Cthun::WebSocket::Connection::OnMessage_Callback onMessage_c =
        [this](Cthun::WebSocket::Client_Type* client_ptr,
               Cthun::WebSocket::Connection::Ptr connection_ptr_c,
               std::string message) {
            assert(connection_ptr_ == connection_ptr_c);
            handle_message(client_ptr, message);
        };

    auto consecutive_pong_timeouts = std::make_shared<int>(0);
    auto pong_mutex = std::make_shared<std::mutex>();

    Cthun::WebSocket::Connection::Pong_Callback onPong_c =
        [consecutive_pong_timeouts, pong_mutex](
                Cthun::WebSocket::Client_Type* client_ptr,
                Cthun::WebSocket::Connection::Ptr connection_ptr_c,
                std::string binary_payload) {
            LOG_DEBUG("received pong - payload: '%1%'", binary_payload);
            if (*consecutive_pong_timeouts > 0){
                std::lock_guard<std::mutex> lock { *pong_mutex };
                *consecutive_pong_timeouts = 0;
            }
        };

    Cthun::WebSocket::Connection::Pong_Callback onPongTimeout_c =
        [consecutive_pong_timeouts, pong_mutex](
                Cthun::WebSocket::Client_Type* client_ptr,
                Cthun::WebSocket::Connection::Ptr connection_ptr_c,
                std::string binary_payload) {
            std::lock_guard<std::mutex> lock { *pong_mutex };
            LOG_WARNING("pong timeout (%1% consecutive) - payload: '%2%'",
                        *consecutive_pong_timeouts, binary_payload);
            (*consecutive_pong_timeouts)++;
        };

    connection_ptr_->setOnOpenCallback(onOpen_c);
    connection_ptr_->setOnMessageCallback(onMessage_c);
    connection_ptr_->setOnPongCallback(onPong_c);
    connection_ptr_->setOnPongTimeoutCallback(onPongTimeout_c);
}

void AgentEndpoint::monitorConnectionState() {
    while (1) {
        if (connection_ptr_->getState()
                != Cthun::WebSocket::Connection_State_Values::open) {
            LOG_WARNING("agent is not connected; will try to reconnect in 2 s");
            sleep(2);

            try {
                Cthun::WebSocket::CONNECTION_MANAGER.open(connection_ptr_);
                connection_ptr_->waitForOpen();
            } catch (Cthun::WebSocket::connection_error& e) {
                LOG_WARNING("failed to reconnect; %1%", e.what());
                throw fatal_error { "failed to reconnect" };
            }
        }

        sleep(11);
    }
}

}  // namespace Agent
}  // namespace Cthun
