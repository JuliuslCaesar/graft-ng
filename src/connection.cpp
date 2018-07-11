#include "connection.h"

namespace graft {

void static_empty_ev_handler(mg_connection *nc, int ev, void *ev_data)
{

}

void UpstreamSender::send(TaskManager &manager, BaseTaskPtr bt)
{
    m_bt = bt;

    const ConfigOpts& opts = manager.getCopts();
    std::string default_uri = opts.cryptonode_rpc_address.c_str();
    Output& output = bt->getOutput();
    std::string url = output.makeUri(default_uri);
    std::string extra_headers = output.combine_headers();
    if(extra_headers.empty())
    {
        extra_headers = "Content-Type: application/json\r\n";
    }
    std::string& body = output.body;
    m_crypton = mg_connect_http(manager.getMgMgr(), static_ev_handler<UpstreamSender>, url.c_str(),
                             extra_headers.c_str(),
                             (body.empty())? nullptr : body.c_str()); //last nullptr means GET
    assert(m_crypton);
    m_crypton->user_data = this;
    mg_set_timer(m_crypton, mg_time() + opts.upstream_request_timeout);
}

void UpstreamSender::ev_handler(mg_connection *crypton, int ev, void *ev_data)
{
    assert(crypton == this->m_crypton);
    switch (ev)
    {
    case MG_EV_CONNECT:
    {
        int& err = *static_cast<int*>(ev_data);
        if(err != 0)
        {
            std::ostringstream ss;
            ss << "cryptonode connect failed: " << strerror(err);
            setError(Status::Error, ss.str().c_str());
            TaskManager::from(crypton->mgr)->onCryptonDone(*this);
            crypton->handler = static_empty_ev_handler;
            releaseItself();
        }
    } break;
    case MG_EV_HTTP_REPLY:
    {
        mg_set_timer(crypton, 0);
        http_message* hm = static_cast<http_message*>(ev_data);
        m_bt->getInput() = *hm;
        setError(Status::Ok);
        crypton->flags |= MG_F_CLOSE_IMMEDIATELY;
        TaskManager::from(crypton->mgr)->onCryptonDone(*this);
        crypton->handler = static_empty_ev_handler;
        releaseItself();
    } break;
    case MG_EV_CLOSE:
    {
        mg_set_timer(crypton, 0);
        setError(Status::Error, "cryptonode connection unexpectedly closed");
        TaskManager::from(crypton->mgr)->onCryptonDone(*this);
        crypton->handler = static_empty_ev_handler;
        releaseItself();
    } break;
    case MG_EV_TIMER:
    {
        mg_set_timer(crypton, 0);
        setError(Status::Error, "cryptonode request timout");
        crypton->flags |= MG_F_CLOSE_IMMEDIATELY;
        TaskManager::from(crypton->mgr)->onCryptonDone(*this);
        crypton->handler = static_empty_ev_handler;
        releaseItself();
    } break;
    default:
        break;
    }
}

constexpr std::pair<const char *, int> ConnectionManager::m_methods[];

ConnectionManager* ConnectionManager::from_accepted(mg_connection *cn)
{
    assert(cn->user_data);
    return static_cast<ConnectionManager*>(cn->user_data);
}

void ConnectionManager::ev_handler_empty(mg_connection *client, int ev, void *ev_data)
{
}

void HttpConnectionManager::bind(TaskManager& manager)
{
    assert(!manager.ready());
    mg_mgr* mgr = manager.getMgMgr();

    const ConfigOpts& opts = manager.getCopts();

    mg_connection *nc_http = mg_bind(mgr, opts.http_address.c_str(), ev_handler_http);
    nc_http->user_data = this;
    mg_set_protocol_http_websocket(nc_http);
}

void CoapConnectionManager::bind(TaskManager& manager)
{
    assert(!manager.ready());
    mg_mgr* mgr = manager.getMgMgr();

    const ConfigOpts& opts = manager.getCopts();

    mg_connection *nc_coap = mg_bind(mgr, opts.coap_address.c_str(), ev_handler_coap);
    nc_coap->user_data = this;
    mg_set_protocol_coap(nc_coap);
}

int HttpConnectionManager::translateMethod(const char *method, std::size_t len)
{
    for (const auto& m : m_methods)
    {
        if (::strncmp(m.first, method, len) == 0)
            return m.second;
    }
    return -1;
}

int CoapConnectionManager::translateMethod(int i)
{
    constexpr int size = sizeof(m_methods)/sizeof(m_methods[0]);
    assert(i<size);
    return m_methods[i].second;
}

void HttpConnectionManager::ev_handler_http(mg_connection *client, int ev, void *ev_data)
{
    TaskManager* manager = TaskManager::from(client->mgr);

    switch (ev)
    {
    case MG_EV_HTTP_REQUEST:
    {
        mg_set_timer(client, 0);

        struct http_message *hm = (struct http_message *) ev_data;
        std::string uri(hm->uri.p, hm->uri.len);
        // TODO: why this is hardcoded ?
        if(uri == "/root/exit")
        {
            manager->stop();
            return;
        }
        int method = translateMethod(hm->method.p, hm->method.len);
        if (method < 0) return;

        HttpConnectionManager* httpcm = HttpConnectionManager::from_accepted(client);
        Router::JobParams prms;
        if (httpcm->matchRoute(uri, method, prms))
        {
            mg_str& body = hm->body;
            prms.input.load(body.p, body.len);
            BaseTask* bt = BaseTask::Create<ClientTask>(httpcm, client, prms).get();
            assert(dynamic_cast<ClientTask*>(bt));
            ClientTask* ptr = static_cast<ClientTask*>(bt);

            client->user_data = ptr;
            client->handler = static_ev_handler<ClientTask>;

            manager->onNewClient(ptr->getSelf());
        }
        else
        {
            mg_http_send_error(client, 500, "invalid parameter");
            client->flags |= MG_F_SEND_AND_CLOSE;
        }
        break;
    }
    case MG_EV_ACCEPT:
    {
        const ConfigOpts& opts = manager->getCopts();

        mg_set_timer(client, mg_time() + opts.http_connection_timeout);
        break;
    }
    case MG_EV_TIMER:
        mg_set_timer(client, 0);
        client->handler = ev_handler_empty; //without this we will get MG_EV_HTTP_REQUEST
        client->flags |= MG_F_CLOSE_IMMEDIATELY;
        break;

    default:
        break;
    }
}

void CoapConnectionManager::ev_handler_coap(mg_connection *client, int ev, void *ev_data)
{
    uint32_t res;
    std::string uri;
    struct mg_coap_message *cm = (struct mg_coap_message *) ev_data;

    if (ev >= MG_COAP_EVENT_BASE)
      if (!cm || cm->code_class != MG_COAP_CODECLASS_REQUEST) return;

    switch (ev)
    {
    case MG_EV_COAP_CON:
        res = mg_coap_send_ack(client, cm->msg_id);
        // No break
    case MG_EV_COAP_NOC:
    {
        struct mg_coap_option *opt = cm->options;
#define COAP_OPT_URI 11
        for (; opt; opt = opt->next)
        {
            if (opt->number = COAP_OPT_URI)
            {
                uri += "/";
                uri += std::string(opt->value.p, opt->value.len);
            }
        }

        int method = translateMethod(cm->code_detail - 1);

        CoapConnectionManager* coapcm = CoapConnectionManager::from_accepted(client);
        Router::JobParams prms;
        if (coapcm->matchRoute(uri, method, prms))
        {
            mg_str& body = cm->payload;
            prms.input.load(body.p, body.len);

            BaseTask* rb_ptr = BaseTask::Create<ClientTask>(coapcm, client, prms).get();
            assert(dynamic_cast<ClientTask*>(rb_ptr));
            ClientTask* ptr = static_cast<ClientTask*>(rb_ptr);

            client->user_data = ptr;
            client->handler = static_ev_handler<ClientTask>;

            TaskManager* manager = TaskManager::from(client->mgr);
            manager->onNewClient(ptr->getSelf());
        }
        break;
    }
    case MG_EV_COAP_ACK:
    case MG_EV_COAP_RST:
        break;
    default:
        break;
    }
}

void ConnectionManager::respond(ClientTask* ct, const std::string& s)
{
    int code;
    switch(ct->m_ctx.local.getLastStatus())
    {
    case Status::Ok: code = 200; break;
    case Status::InternalError:
    case Status::Error: code = 500; break;
    case Status::Busy: code = 503; break;
    case Status::Drop: code = 400; break;
    default: assert(false); break;
    }

    auto& m_ctx = ct->m_ctx;
    auto& m_client = ct->m_client;
    if(Status::Ok == m_ctx.local.getLastStatus())
    {
        mg_send_head(m_client, code, s.size(), "Content-Type: application/json\r\nConnection: close");
        mg_send(m_client, s.c_str(), s.size());
    }
    else
    {
        mg_http_send_error(m_client, code, s.c_str());
    }
    m_client->flags |= MG_F_SEND_AND_CLOSE;
    m_client->handler = static_empty_ev_handler;
    m_client = nullptr;
}

}//namespace graft