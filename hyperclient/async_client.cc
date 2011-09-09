// Copyright (c) 2011, Cornell University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright notice,
//       this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of HyperDex nor the names of its contributors may be
//       used to endorse or promote products derived from this software without
//       specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// STL
#include <queue>

// po6
#include <po6/net/socket.h>

// e
#include <e/bitfield.h>
#include <e/intrusive_ptr.h>

// Utils
#include "hashing.h"

// HyperDex
#include <hyperdex/configuration.h>
#include <hyperdex/coordinatorlink.h>
#include <hyperdex/network_constants.h>

// HyperClient
#include <hyperclient/async_client.h>

namespace hyperclient
{

class channel
{
    public:
        channel(const hyperdex::instance& inst);
        ~channel() throw ();

    public:
        po6::net::socket soc;
        uint64_t nonce;
        hyperdex::entityid id;

    private:
        friend class e::intrusive_ptr<channel>;

    private:
        void inc() { ++m_ref; }
        void dec() { if (--m_ref == 0) delete this; }

    private:
        size_t m_ref;
};

class pending
{
    public:
        pending();
        virtual ~pending() throw ();

    public:
        virtual void result(returncode ret) = 0;
        virtual e::intrusive_ptr<pending> result(returncode status,
                                                 hyperdex::network_msgtype msg_type,
                                                 const e::buffer& msg) = 0;

    public:
        e::intrusive_ptr<channel> chan;
        hyperdex::entityid ent;
        hyperdex::instance inst;
        uint32_t nonce;

    private:
        friend class e::intrusive_ptr<pending>;

    private:
        void inc() { ++m_ref; }
        void dec() { if (--m_ref == 0) delete this; }

    private:
        size_t m_ref;
};

class pending_get : public pending
{
    public:
        pending_get(std::tr1::function<void (returncode, const std::vector<e::buffer>&)> callback);
        virtual ~pending_get() throw ();

    public:
        virtual void result(returncode ret);
        virtual e::intrusive_ptr<pending> result(returncode status,
                                                 hyperdex::network_msgtype msg_type,
                                                 const e::buffer& msg);

    private:
        std::tr1::function<void (returncode, const std::vector<e::buffer>&)> m_callback;
};

class pending_mutate : public pending
{
    public:
        pending_mutate(hyperdex::network_msgtype expected,
                       std::tr1::function<void (returncode)> callback);
        virtual ~pending_mutate() throw ();

    public:
        virtual void result(returncode ret);
        virtual e::intrusive_ptr<pending> result(returncode status,
                                                 hyperdex::network_msgtype msg_type,
                                                 const e::buffer& msg);

    private:
        hyperdex::network_msgtype m_expected;
        std::tr1::function<void (returncode)> m_callback;
};

class async_client_impl : public hyperclient :: async_client
{
    public:
        async_client_impl(po6::net::location coordinator);
        virtual ~async_client_impl() throw ();

    public:
        virtual returncode connect();

    public:
        virtual void get(const std::string& space, const e::buffer& key,
                         std::tr1::function<void (returncode, const std::vector<e::buffer>&)> callback);
        virtual void put(const std::string& space, const e::buffer& key,
                         const std::vector<e::buffer>& value,
                         std::tr1::function<void (returncode)> callback);
        virtual void del(const std::string& space, const e::buffer& key,
                         std::tr1::function<void (returncode)> callback);
        virtual void update(const std::string& space, const e::buffer& key,
                            const std::map<std::string, e::buffer>& value,
                            std::tr1::function<void (returncode)> callback);
        returncode flush();

    public:
        void add_reqrep(const std::string&, const e::buffer& key,
                        hyperdex::network_msgtype send_type,
                        const e::buffer& send_msg, e::intrusive_ptr<pending> op);
        bool send(e::intrusive_ptr<channel> chan,
                  e::intrusive_ptr<pending> op,
                  const hyperdex::entityid& entity,
                  const hyperdex::instance& inst,
                  uint32_t nonce,
                  hyperdex::network_msgtype send_type,
                  const e::buffer& send_msg);

    private:
        bool m_initialized;
        hyperdex::coordinatorlink m_coord;
        hyperdex::configuration m_config;
        std::map<hyperdex::instance, e::intrusive_ptr<channel> > m_channels;
        std::deque<e::intrusive_ptr<pending> > m_requests;
};

} // hyperclient

hyperclient::async_client*
hyperclient :: async_client :: create(po6::net::location coordinator)
{
    return new async_client_impl(coordinator);
}

hyperclient :: async_client :: ~async_client() throw ()
{
}

hyperclient :: async_client_impl :: async_client_impl(po6::net::location coordinator)
    : m_initialized(false)
    , m_coord(coordinator)
    , m_config()
    , m_channels()
    , m_requests()
{
    m_coord.set_announce("client");
}

hyperclient :: async_client_impl :: ~async_client_impl() throw ()
{
}

hyperclient::returncode
hyperclient :: async_client_impl :: connect()
{
    switch (m_coord.connect())
    {
        case hyperdex::coordinatorlink::SUCCESS:
            break;
        case hyperdex::coordinatorlink::CONNECTFAIL:
            return COORDFAIL;
        case hyperdex::coordinatorlink::DISCONNECT:
        case hyperdex::coordinatorlink::SHUTDOWN:
        case hyperdex::coordinatorlink::LOGICERROR:
        default:
            return LOGICERROR;
    }

    while (true)
    {
        switch (m_coord.loop(1, -1))
        {
            case hyperdex::coordinatorlink::SUCCESS:
                break;
            case hyperdex::coordinatorlink::CONNECTFAIL:
                return COORDFAIL;
            case hyperdex::coordinatorlink::DISCONNECT:
                return COORDFAIL;
            case hyperdex::coordinatorlink::SHUTDOWN:
            case hyperdex::coordinatorlink::LOGICERROR:
            default:
                return LOGICERROR;
        }

        if (m_coord.unacknowledged())
        {
            m_config = m_coord.config();
            m_coord.acknowledge();
            break;
        }
    }

    return SUCCESS;
}

void
hyperclient :: async_client_impl :: get(const std::string& space,
                                        const e::buffer& key,
                                        std::tr1::function<void (returncode, const std::vector<e::buffer>&)> callback)
{
    e::intrusive_ptr<pending> op = new pending_get(callback);
    add_reqrep(space, key, hyperdex::REQ_GET, key, op);
}

void
hyperclient :: async_client_impl :: put(const std::string& space,
                                        const e::buffer& key,
                                        const std::vector<e::buffer>& value,
                                        std::tr1::function<void (returncode)> callback)
{
    e::buffer msg;
    msg.pack() << key << value;
    e::intrusive_ptr<pending> op = new pending_mutate(hyperdex::RESP_PUT, callback);
    add_reqrep(space, key, hyperdex::REQ_PUT, msg, op);
}

void
hyperclient :: async_client_impl :: del(const std::string& space,
                                        const e::buffer& key,
                                        std::tr1::function<void (returncode)> callback)
{
    e::intrusive_ptr<pending> op = new pending_mutate(hyperdex::RESP_DEL, callback);
    add_reqrep(space, key, hyperdex::REQ_DEL, key, op);
}

void
hyperclient :: async_client_impl :: update(const std::string& space,
                                           const e::buffer& key,
                                           const std::map<std::string, e::buffer>& value,
                                           std::tr1::function<void (returncode)> callback)
{
    hyperdex::spaceid si = m_config.lookup_spaceid(space);

    if (si == hyperdex::configuration::NULLSPACE)
    {
        callback(NOTASPACE);
        return;
    }

    std::vector<std::string> dimension_names = m_config.lookup_space_dimensions(si);
    assert(dimension_names.size() > 0);

    e::bitfield bits(dimension_names.size() - 1);
    std::vector<e::buffer> realvalue(dimension_names.size() - 1);
    std::set<std::string> seen;

    for (size_t i = 1; i < dimension_names.size(); ++i)
    {
        std::map<std::string, e::buffer>::const_iterator valiter;
        valiter = value.find(dimension_names[i]);

        if (valiter == value.end())
        {
            bits.unset(i - 1);
        }
        else
        {
            seen.insert(valiter->first);
            bits.set(i - 1);
            realvalue[i - 1] = valiter->second;
        }
    }

    for (std::map<std::string, e::buffer>::const_iterator i = value.begin();
            i != value.end(); ++i)
    {
        if (seen.find(i->first) == seen.end())
        {
            callback(BADDIMENSION);
            return;
        }
    }

    e::buffer msg;
    msg.pack() << key << bits << realvalue;
    e::intrusive_ptr<pending> op = new pending_mutate(hyperdex::RESP_UPDATE, callback);
    add_reqrep(space, key, hyperdex::REQ_UPDATE, msg, op);
}

void
hyperclient :: async_client_impl :: add_reqrep(const std::string& space,
                                               const e::buffer& key,
                                               hyperdex::network_msgtype send_type,
                                               const e::buffer& send_msg,
                                               e::intrusive_ptr<pending> op)
{
    hyperdex::spaceid si = m_config.lookup_spaceid(space);

    if (si == hyperdex::configuration::NULLSPACE)
    {
        op->result(NOTASPACE);
        return;
    }

    // Figure out who to talk with.
    hyperdex::regionid point_leader(si.space, 0, 64, CityHash64(key));
    hyperdex::entityid dst_ent = m_config.headof(point_leader);
    hyperdex::instance dst_inst = m_config.instancefor(dst_ent);

    if (dst_inst == hyperdex::configuration::NULLINSTANCE)
    {
        op->result(CONNECTFAIL);
        return;
    }

    e::intrusive_ptr<channel> chan = m_channels[dst_inst];

    if (!chan)
    {
        try
        {
            m_channels[dst_inst] = chan = new channel(dst_inst);
        }
        catch (po6::error& e)
        {
            op->result(CONNECTFAIL);
            return;
        }
    }

    uint32_t nonce = chan->nonce;
    ++chan->nonce;
    op->chan = chan;
    op->ent = dst_ent;
    op->inst = dst_inst;
    op->nonce = nonce;
    m_requests.push_back(op);

    if (!send(chan, op, dst_ent, dst_inst, nonce, send_type, send_msg))
    {
        m_requests.pop_back();
    }
}

bool
hyperclient :: async_client_impl :: send(e::intrusive_ptr<channel> chan,
                                         e::intrusive_ptr<pending> op,
                                         const hyperdex::entityid& ent,
                                         const hyperdex::instance& inst,
                                         uint32_t nonce,
                                         hyperdex::network_msgtype send_type,
                                         const e::buffer& send_msg)
{
    const uint8_t type = static_cast<uint8_t>(send_type);
    const uint16_t fromver = 0;
    const uint16_t tover = inst.inbound_version;
    const hyperdex::entityid& from(chan->id);
    const hyperdex::entityid& to(ent);
    const uint32_t size = sizeof(uint32_t) + sizeof(type) + sizeof(fromver)
                        + sizeof(tover) + hyperdex::entityid::SERIALIZEDSIZE * 2
                        + send_msg.size();
    e::buffer packed(size);
    packed.pack() << size << type
                  << fromver << tover
                  << from << to
                  << nonce;
    packed += send_msg;

    try
    {
        chan->soc.xsend(packed.get(), packed.size(), MSG_NOSIGNAL);
    }
    catch (po6::error& e)
    {
        m_channels.erase(inst);
        op->result(DISCONNECT);
        return false;
    }

    return true;
}

hyperclient::returncode
hyperclient :: async_client_impl :: flush()
{
    while (!m_requests.empty())
    {
        for (int i = 0; i < 7 && !m_coord.connected(); ++i)
        {
            switch (m_coord.connect())
            {
                case hyperdex::coordinatorlink::SUCCESS:
                    break;
                case hyperdex::coordinatorlink::CONNECTFAIL:
                case hyperdex::coordinatorlink::DISCONNECT:

                    if (i == 6)
                    {
                        return COORDFAIL;
                    }

                    break;
                case hyperdex::coordinatorlink::SHUTDOWN:
                case hyperdex::coordinatorlink::LOGICERROR:
                default:

                    if (i == 6)
                    {
                        return LOGICERROR;
                    }

                    break;
            }
        }

        size_t num_pfds = m_requests.size();
        std::vector<pollfd> pfds(num_pfds + 1);

        for (size_t i = 0; i < num_pfds; ++i)
        {
            if (m_requests[i])
            {
                pfds[i].fd = m_requests[i]->chan->soc.get();
            }
            else
            {
                pfds[i].fd = -1;
            }

            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }

        pfds[num_pfds] = m_coord.pfd();
        pfds[num_pfds].revents = 0;

        if (poll(&pfds.front(), num_pfds + 1, -1) < 0)
        {
            return LOGICERROR;
        }

        if (pfds[num_pfds].revents != 0)
        {
            switch (m_coord.loop(1, 0))
            {
                case hyperdex::coordinatorlink::SUCCESS:
                    break;
                case hyperdex::coordinatorlink::CONNECTFAIL:
                case hyperdex::coordinatorlink::DISCONNECT:
                    return COORDFAIL;
                case hyperdex::coordinatorlink::SHUTDOWN:
                case hyperdex::coordinatorlink::LOGICERROR:
                default:
                    return LOGICERROR;
            }
        }

        if (m_coord.unacknowledged())
        {
            m_config = m_coord.config();
            m_coord.acknowledge();

            for (std::deque<e::intrusive_ptr<pending> >::iterator i = m_requests.begin();
                    i != m_requests.end(); ++i)
            {
                if (*i && m_config.instancefor((*i)->ent) != (*i)->inst)
                {
                    (*i)->result(RECONFIGURE);
                    *i = NULL;
                }
            }

            continue;
        }

        for (size_t i = 0; i < num_pfds; ++i)
        {
            if (!m_requests[i])
            {
                continue;
            }

            if ((pfds[i].revents & POLLHUP) || (pfds[i].revents & POLLERR))
            {
                m_requests[i]->chan->soc.close();
                m_channels.erase(m_requests[i]->inst);
                m_requests[i]->result(DISCONNECT);
                m_requests[i] = NULL;
                continue;
            }

            if (!(pfds[i].revents & POLLIN))
            {
                continue;
            }

            e::intrusive_ptr<channel> chan = m_requests[i]->chan;

            if (chan->soc.get() < 0)
            {
                m_requests[i]->result(DISCONNECT);
                m_requests[i] = NULL;
                continue;
            }

            try
            {
                uint32_t size;

                if (recv(chan->soc.get(), &size, 4, MSG_DONTWAIT|MSG_PEEK) != 4)
                {
                    continue;
                }

                size = be32toh(size);
                size += sizeof(uint32_t);
                e::buffer response(size);

                if (xread(&chan->soc, &response, size) < size)
                {
                    chan->soc.close();
                    m_channels.erase(m_requests[i]->inst);
                    m_requests[i]->result(DISCONNECT);
                    m_requests[i] = NULL;
                    continue;
                }

                uint32_t nop;
                uint8_t type_num;
                uint16_t fromver;
                uint16_t tover;
                hyperdex::entityid from;
                hyperdex::entityid to;
                uint32_t nonce;
                e::unpacker up(response.unpack());
                up >> nop >> type_num >> fromver >> tover >> from >> to >> nonce;
                hyperdex::network_msgtype msg_type = static_cast<hyperdex::network_msgtype>(type_num);

                if (chan->id == hyperdex::entityid(hyperdex::configuration::CLIENTSPACE))
                {
                    chan->id = to;
                }

                e::buffer msg;
                up.leftovers(&msg);

                for (std::deque<e::intrusive_ptr<pending> >::iterator req = m_requests.begin();
                        req != m_requests.end(); ++req)
                {
                    if (*req &&
                        chan == (*req)->chan &&
                        fromver == (*req)->inst.inbound_version &&
                        tover == 0 &&
                        from == (*req)->ent &&
                        to == chan->id &&
                        nonce == (*req)->nonce)
                    {
                        *req = (*req)->result(SUCCESS, msg_type, msg);
                    }
                }
            }
            catch (po6::error& e)
            {
                m_requests[i]->chan->soc.close();
                m_channels.erase(m_requests[i]->inst);
                m_requests[i]->result(DISCONNECT);
                m_requests[i] = NULL;
            }
            catch (std::out_of_range& e)
            {
                m_requests[i]->chan->soc.close();
                m_channels.erase(m_requests[i]->inst);
                m_requests[i]->result(DISCONNECT);
                m_requests[i] = NULL;
            }
        }

        while (!m_requests.empty() && !m_requests.front())
        {
            m_requests.pop_front();
        }
    }

    return SUCCESS;
}

hyperclient :: channel :: channel(const hyperdex::instance& inst)
    : soc(inst.inbound.address.family(), SOCK_STREAM, IPPROTO_TCP)
    , nonce(1)
    , id(hyperdex::configuration::CLIENTSPACE)
    , m_ref(0)
{
    soc.connect(inst.inbound);
}

hyperclient :: channel :: ~channel() throw ()
{
}

hyperclient :: pending :: pending()
    : chan()
    , ent()
    , inst()
    , nonce()
    , m_ref(0)
{
}

hyperclient :: pending :: ~pending() throw ()
{
}

hyperclient :: pending_get :: pending_get(std::tr1::function<void (returncode, const std::vector<e::buffer>&)> callback)
    : m_callback(callback)
{
}

hyperclient :: pending_get :: ~pending_get() throw ()
{
}

void
hyperclient :: pending_get :: result(returncode ret)
{
    std::vector<e::buffer> res;
    m_callback(ret, res);
}

e::intrusive_ptr<hyperclient::pending>
hyperclient :: pending_get :: result(returncode ret,
                                     hyperdex::network_msgtype msg_type,
                                     const e::buffer& msg)
{
    std::vector<e::buffer> value;

    if (ret != SUCCESS)
    {
        m_callback(ret, value);
        return NULL;
    }

    if (msg_type != hyperdex::RESP_GET)
    {
        m_callback(SERVERERROR, value);
        return NULL;
    }

    try
    {
        e::unpacker up(msg);
        uint16_t response;
        up >> response;

        switch (static_cast<hyperdex::network_returncode>(response))
        {
            case hyperdex::NET_SUCCESS:
                up >> value;
                m_callback(SUCCESS, value);
                break;
            case hyperdex::NET_NOTFOUND:
                m_callback(NOTFOUND, value);
                break;
            case hyperdex::NET_WRONGARITY:
                m_callback(WRONGARITY, value);
                break;
            case hyperdex::NET_NOTUS:
                m_callback(LOGICERROR, value);
                break;
            case hyperdex::NET_SERVERERROR:
                m_callback(SERVERERROR, value);
                break;
            default:
                m_callback(SERVERERROR, value);
                break;
        }
    }
    catch (std::out_of_range& e)
    {
        m_callback(SERVERERROR, value);
    }

    return NULL;
}

hyperclient :: pending_mutate :: pending_mutate(hyperdex::network_msgtype expected,
                                                std::tr1::function<void (returncode)> callback)
    : m_expected(expected)
    , m_callback(callback)
{
}

hyperclient :: pending_mutate :: ~pending_mutate() throw ()
{
}

void
hyperclient :: pending_mutate :: result(returncode ret)
{
    m_callback(ret);
}

e::intrusive_ptr<hyperclient::pending>
hyperclient :: pending_mutate :: result(returncode ret,
                                        hyperdex::network_msgtype msg_type,
                                        const e::buffer& msg)
{
    if (ret != SUCCESS)
    {
        m_callback(ret);
        return NULL;
    }

    if (msg_type != m_expected)
    {
        m_callback(SERVERERROR);
        return NULL;
    }

    try
    {
        e::unpacker up(msg);
        uint16_t response;
        up >> response;

        switch (static_cast<hyperdex::network_returncode>(response))
        {
            case hyperdex::NET_SUCCESS:
                m_callback(SUCCESS);
                break;
            case hyperdex::NET_NOTFOUND:
                m_callback(NOTFOUND);
                break;
            case hyperdex::NET_WRONGARITY:
                m_callback(WRONGARITY);
                break;
            case hyperdex::NET_NOTUS:
                m_callback(LOGICERROR);
                break;
            case hyperdex::NET_SERVERERROR:
                m_callback(SERVERERROR);
                break;
            default:
                m_callback(SERVERERROR);
                break;
        }
    }
    catch (std::out_of_range& e)
    {
        m_callback(SERVERERROR);
    }

    return NULL;
}