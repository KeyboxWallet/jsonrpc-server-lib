#include <nlohmann/json.hpp>

#ifndef JSON_RPC_INTERFACE_INCLUDE
#define JSON_RPC_INTERFACE_INCLUDE


using json = nlohmann::json;



class generic_json_rpc_session {
    public:
     virtual void do_reply(const json & id, const json & o) = 0;
     virtual void do_notify( const std::string method_name, const json &o) = 0;
};

class json_rpc_context_free_server {
    public:
    virtual json call(std::string method_name, const json &params  ) = 0;
};

class json_rpc_context_server {
    public:
    virtual void session_added(generic_json_rpc_session * session) = 0;
    virtual void session_removed(generic_json_rpc_session *session) = 0;
    virtual void call(generic_json_rpc_session *session, const json &id, const std::string method_name, const json& params  ) = 0;
};

#endif
