#pragma once
#include <dlfcn.h>
#include <exception>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
#include <iostream>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <wan_agent/logger.hpp>

namespace wan_agent
{

    void WanAgent::load_config() noexcept(false)
    {
        log_enter_func();
        // Check if all mandatory keys are included.
        static const std::vector<std::string> must_have{
            WAN_AGENT_CONF_VERSION,
            // WAN_AGENT_CONF_TRANSPORT, // not so mandatory now.
            WAN_AGENT_CONF_LOCAL_SITE_ID,
            WAN_AGENT_CONF_SERVER_SITES,
            WAN_AGENT_CONF_SENDER_SITES,
            // we need to get local ip & port info directly
            WAN_AGENT_CONF_PRIVATE_IP,
            WAN_AGENT_CONF_PRIVATE_PORT};
        for (auto &must_have_key : must_have)
        {
            std::cout << must_have_key << std::endl;
            if (config.find(must_have_key) == config.end())
            {
                throw std::runtime_error(must_have_key + " is not found");
            }
        }
        local_site_id = config[WAN_AGENT_CONF_LOCAL_SITE_ID];
        local_ip = config[WAN_AGENT_CONF_PRIVATE_IP];
        local_port = config[WAN_AGENT_CONF_PRIVATE_PORT];
        // Check if sites are valid.
        if (config[WAN_AGENT_CONF_SENDER_SITES].size() == 0 || config[WAN_AGENT_CONF_SERVER_SITES] ==0)
        {
            throw std::runtime_error("Sites do not have any configuration");
        }
        for (auto &site : config[WAN_AGENT_CONF_SENDER_SITES])
        {
            WAN_AGENT_CHECK_SITE_ENTRY(WAN_AGENT_CONF_SITES_ID);
            WAN_AGENT_CHECK_SITE_ENTRY(WAN_AGENT_CONF_SITES_IP);
            WAN_AGENT_CHECK_SITE_ENTRY(WAN_AGENT_CONF_SITES_PORT);
            sender_sites_ip_addrs_and_ports.emplace(site[WAN_AGENT_CONF_SITES_ID],
                                                    std::make_pair(site[WAN_AGENT_CONF_SITES_IP],
                                                                   site[WAN_AGENT_CONF_SITES_PORT]));
        }
        for (auto &site : config[WAN_AGENT_CONF_SERVER_SITES])
        {
            WAN_AGENT_CHECK_SITE_ENTRY(WAN_AGENT_CONF_SITES_ID);
            WAN_AGENT_CHECK_SITE_ENTRY(WAN_AGENT_CONF_SITES_IP);
            WAN_AGENT_CHECK_SITE_ENTRY(WAN_AGENT_CONF_SITES_PORT);
            server_sites_ip_addrs_and_ports.emplace(site[WAN_AGENT_CONF_SITES_ID],
                                                    std::make_pair(site[WAN_AGENT_CONF_SITES_IP],
                                                                   site[WAN_AGENT_CONF_SITES_PORT]));
        }
        log_exit_func();
    } // namespace wan_agent

    std::string WanAgent::get_local_ip_and_port() noexcept(false)
    {
        std::string local_ip;
        unsigned short local_port = 0;
        if (config.find(WAN_AGENT_CONF_PRIVATE_IP) != config.end() &&
            config.find(WAN_AGENT_CONF_PRIVATE_PORT) != config.end())
        {
            local_ip = config[WAN_AGENT_CONF_PRIVATE_IP];
            local_port = config[WAN_AGENT_CONF_PRIVATE_PORT];
        }
        else
        {
            throw std::runtime_error("Cannot find ip and port configuration for local site.");
        }
        return local_ip + ":" + std::to_string(local_port);
    }

    WanAgent::WanAgent(const nlohmann::json &wan_group_config)
        : is_shutdown(false),
          config(wan_group_config)
    {
        // this->message_counters = std::make_unique<std::map<uint32_t,std::atomic<uint64_t>>>();
        load_config();
    }

    RemoteMessageService::RemoteMessageService(const site_id_t local_site_id,
                                               int num_senders,
                                               unsigned short local_port,
                                               const size_t max_payload_size,
                                               const RemoteMessageCallback &rmc,
                                               const NotifierFunc &ready_notifier_lambda)
        : local_site_id(local_site_id),
          num_senders(num_senders),
          max_payload_size(max_payload_size),
          rmc(rmc),
          ready_notifier(ready_notifier_lambda),
          server_ready(false)
    {
        std::cout << "1: " << local_site_id << std::endl;
        std::cout << "2" << std::endl;
        sockaddr_in serv_addr;
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            throw std::runtime_error("RemoteMessageService failed to create socket.");

        int reuse_addr = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse_addr,
                       sizeof(reuse_addr)) < 0)
        {
            fprintf(stderr, "ERROR on setsockopt: %s\n", strerror(errno));
        }

        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = INADDR_ANY;
        serv_addr.sin_port = htons(local_port);
        if (bind(fd, (sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        {
            fprintf(stderr, "ERROR on binding to socket: %s\n", strerror(errno));
            throw std::runtime_error("RemoteMessageService failed to bind socket.");
        }
        listen(fd, 5);
        server_socket = fd;
        std::cout << "RemoteMessageService listening on " << local_port << std::endl;
        // dbg_default_info("RemoteMessageService listening on {} ...", local_port);
    };

    void RemoteMessageService::establish_connections()
    {

        // TODO: maybe support dynamic join later, i.e. having a infinite loop always listening for join requests?
        while (worker_threads.size() < num_senders)
        {
            struct sockaddr_storage client_addr_info;
            socklen_t len = sizeof client_addr_info;

            int sock = ::accept(server_socket, (struct sockaddr *)&client_addr_info, &len);
            worker_threads.emplace_back(std::thread(&RemoteMessageService::worker, this, sock));
        }
        server_ready.store(true);
        ready_notifier();
    }

    void RemoteMessageService::worker(int sock)
    {
        RequestHeader header;
        bool success;
        std::unique_ptr<char[]> buffer = std::make_unique<char[]>(max_payload_size);
        std::cout << "worker start" << std::endl;
        while (1)
        {
            if (sock < 0)
                throw std::runtime_error("sock closed!");

            success = sock_read(sock, header);
            if (!success)
                throw std::runtime_error("Failed to read request header");

            success = sock_read(sock, buffer.get(), header.payload_size);
            if (!success)
                throw std::runtime_error("Failed to read message");

            // dbg_default_info("received msg {} from site {}", header.seq, header.site_id);

            rmc(header.site_id, buffer.get(), header.payload_size);
            success = sock_write(sock, Response{header.seq, local_site_id});
            if (!success)
                throw std::runtime_error("Failed to send ACK message");
        }
    }

    bool RemoteMessageService::is_server_ready()
    {
        return server_ready.load();
    }

    WanAgentServer::WanAgentServer(const nlohmann::json &wan_group_config,
                                   const RemoteMessageCallback &rmc)
        : WanAgent(wan_group_config),
          remote_message_callback(rmc), // TODO: server
          remote_message_service(
              local_site_id,
              sender_sites_ip_addrs_and_ports.size(),
              local_port,
              wan_group_config[WAN_AGENT_MAX_PAYLOAD_SIZE],
              rmc,
              [this]() { this->ready_cv.notify_all(); })
    {
        std::thread rms_establish_thread(&RemoteMessageService::establish_connections, &remote_message_service);
        rms_establish_thread.detach();

        // deprecated
        // // TODO: for now, all sites must start in 3 seconds; to be replaced with retry mechanism when establishing sockets
        // sleep(3);

        std::cout << "Press ENTER to kill." << std::endl;
        std::cin.get();
        shutdown_and_wait();
    }

    void WanAgentServer::shutdown_and_wait()
    {
        log_enter_func();
        is_shutdown.store(true);
        log_exit_func();
    }

    MessageSender::MessageSender(const site_id_t &local_site_id,
                                 const std::map<site_id_t, std::pair<ip_addr_t, uint16_t>> &server_sites_ip_addrs_and_ports,
                                 const size_t &n_slots, const size_t &max_payload_size,
                                 std::map<site_id_t, std::atomic<uint64_t>> &message_counters,
                                 const ReportACKFunc &report_new_ack,
                                 const NotifierFunc &ready_notifier_lambda)
        : local_site_id(local_site_id),
          n_slots(n_slots),
          last_all_sent_seqno(static_cast<uint64_t>(-1)),
          message_counters(message_counters),
          report_new_ack(report_new_ack),
          ready_notifier(ready_notifier_lambda),
          client_ready(false),
          thread_shutdown(false)
    {
        log_enter_func();
        // for(unsigned int i = 0; i < n_slots; i++) {
        //     buf.push_back(std::make_unique<char[]>(sizeof(size_t) + max_payload_size));
        // }

        epoll_fd_send_msg = epoll_create1(0);
        if (epoll_fd_send_msg == -1)
            throw std::runtime_error("failed to create epoll fd");

        epoll_fd_recv_ack = epoll_create1(0);
        if (epoll_fd_recv_ack == -1)
            throw std::runtime_error("failed to create epoll fd");

        for (const auto &[site_id, ip_port] : server_sites_ip_addrs_and_ports)
        {
            if (site_id != local_site_id)
            {
                sockaddr_in serv_addr;
                int fd = ::socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0)
                    throw std::runtime_error("MessageSender failed to create socket.");

                memset(&serv_addr, 0, sizeof(serv_addr));
                serv_addr.sin_family = AF_INET;
                serv_addr.sin_port = htons(ip_port.second);

                inet_pton(AF_INET, ip_port.first.c_str(), &serv_addr.sin_addr);
                if (connect(fd, (sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
                {
                    // log_debug("ERROR on connecting to socket: {}", strerror(errno));
                    throw std::runtime_error("MessageSender failed to connect socket");
                }
                add_epoll(epoll_fd_send_msg, EPOLLOUT, fd);
                add_epoll(epoll_fd_recv_ack, EPOLLIN, fd);
                sockfd_to_site_id_map[fd] = site_id;
                last_sent_seqno.emplace(site_id, static_cast<uint64_t>(-1));
            }
            // sockets.emplace(node_id, fd);
        }

        client_ready.store(true);
        ready_notifier();
        log_exit_func();
    }

    void MessageSender::recv_ack_loop()
    {
        struct epoll_event events[EPOLL_MAXEVENTS];
        while (!thread_shutdown)
        {
            int n = epoll_wait(epoll_fd_recv_ack, events, EPOLL_MAXEVENTS, -1);
            for (int i = 0; i < n; i++)
            {
                if (events[i].events & EPOLLIN)
                {
                    // received ACK
                    Response res;
                    sock_read(events[i].data.fd, res);
                    log_info("received ACK from {} for msg {}", res.site_id, res.seq);
                    if (message_counters[res.site_id] != res.seq)
                    {
                        throw std::runtime_error("sequence number is out of order for site-" + std::to_string(res.site_id) + ", counter = " + std::to_string(message_counters[res.site_id].load()) + ", seqno = " + std::to_string(res.seq));
                    }
                    message_counters[res.site_id]++;
                    report_new_ack();
                    // ack_keeper[res.seq * 4 + res.site_id - 1] = now_us();
                }
            }
        }
    }

    void MessageSender::enqueue(const char *payload, const size_t payload_size)
    {
        // std::unique_lock<std::mutex> lock(mutex);
        size_mutex.lock();
        LinkedBufferNode *tmp = new LinkedBufferNode();
        tmp->message_size = payload_size;
        tmp->message_body = (char *)malloc(payload_size);
        memcpy(tmp->message_body, payload, payload_size);
        buffer_list.push_back(*tmp);
        size++;
        size_mutex.unlock();
        not_empty.notify_one();
    }

    void MessageSender::send_msg_loop()
    {
        struct epoll_event events[EPOLL_MAXEVENTS];
        while (!thread_shutdown)
        {
            std::unique_lock<std::mutex> lock(mutex);
            not_empty.wait(lock, [this]() { return size > 0; });
            // has item on the queue to send
            int n = epoll_wait(epoll_fd_send_msg, events, EPOLL_MAXEVENTS, -1);
            // log_trace("epoll returned {} sockets ready for write", n);
            for (int i = 0; i < n; i++)
            {
                if (events[i].events & EPOLLOUT)
                {
                    // socket send buffer is available to send message
                    site_id_t site_id = sockfd_to_site_id_map[events[i].data.fd];
                    // log_trace("send buffer is available for site {}.", site_id);
                    auto offset = last_sent_seqno[site_id] - last_all_sent_seqno;
                    if (offset == size)
                    {
                        // all messages on the buffer have been sent for this site_id
                        continue;
                    }
                    // auto pos = (offset + head) % n_slots;

                    size_t payload_size = buffer_list.front().message_size;
                    // decode paylaod_size in the beginning
                    // memcpy(&payload_size, buf[pos].get(), sizeof(size_t));
                    auto curr_seqno = last_sent_seqno[site_id] + 1;
                    // log_info("sending msg {} to site {}.", curr_seqno, site_id);
                    // send over socket
                    // time_keeper[curr_seqno*4+site_id-1] = now_us();
                    sock_write(events[i].data.fd, RequestHeader{curr_seqno, local_site_id, payload_size});
                    sock_write(events[i].data.fd, buffer_list.front().message_body, payload_size);
                    // buffer_size[curr_seqno] = size;
                    log_trace("buffer has {} items in buffer", size);
                    last_sent_seqno[site_id] = curr_seqno;
                }
            }

            // static_cast<uint64_t>(-1) will simpliy the logic in the above loop
            // but we need to be careful when computing min_element, since it's actually 0xFFFFFFF
            // but we still want -1 to be the min element.
            auto it = std::min_element(last_sent_seqno.begin(), last_sent_seqno.end(),
                                       [](const auto &p1, const auto &p2) { 
                                           if (p1.second == static_cast<uint64_t>(-1)) {return true;} 
                                           else {return p1.second < p2.second;} });

            // log_debug("smallest seqno in last_sent_seqno is {}", it->second);
            // dequeue from ring buffer
            // || min_element == 0 will skip the comparison with static_cast<uint64_t>(-1)
            if (it->second > last_all_sent_seqno || (last_all_sent_seqno == static_cast<uint64_t>(-1) && it->second == 0))
            {
                // log_info("{} has been sent to all remote sites, ", it->second);
                assert(it->second - last_all_sent_seqno == 1);
                // std::unique_lock<std::mutex> list_lock(list_mutex);
                size_mutex.lock();
                buffer_list.pop_front();
                // list_lock.lock();
                size--;
                size_mutex.unlock();
                // list_lock.unlock();
                last_all_sent_seqno++;
            }
            lock.unlock();
        }
    }

    WanAgentSender::WanAgentSender(const nlohmann::json &wan_group_config,
                                   const PredicateLambda &pl)
        : WanAgent(wan_group_config),
          has_new_ack(false),
          predicate_lambda(pl)
    {

        // start predicate thread.
        predicate_thread = std::thread(&WanAgentSender::predicate_loop, this);
        for (const auto &pair : server_sites_ip_addrs_and_ports)
        {
            if (local_site_id != pair.first)
            {
                message_counters[pair.first] = 0;
            }
        }

        message_sender = std::make_unique<MessageSender>(
            local_site_id,
            server_sites_ip_addrs_and_ports,
            wan_group_config[WAN_AGENT_WINDOW_SIZE],
            wan_group_config[WAN_AGENT_MAX_PAYLOAD_SIZE],
            message_counters,
            [this]() { this->report_new_ack(); },
            [this]() { this->ready_cv.notify_all(); });

        recv_ack_thread = std::thread(&MessageSender::recv_ack_loop, message_sender.get());
        send_msg_thread = std::thread(&MessageSender::send_msg_loop, message_sender.get());
    }

    void WanAgentSender::report_new_ack() // TODO: client
    {
        log_enter_func();
        std::unique_lock lck(new_ack_mutex);
        has_new_ack = true;
        lck.unlock();
        new_ack_cv.notify_all();
        log_exit_func();
    }

    void WanAgentSender::predicate_loop() // TODO: client
    {
        log_enter_func();
        while (!is_shutdown.load())
        {
            std::unique_lock lck(new_ack_mutex);
            new_ack_cv.wait(lck, [this]() { return this->has_new_ack; });
            if (is_shutdown.load())
            {
                break;
            }
            std::map<site_id_t, uint64_t> mcs = std::move(get_message_counters());
            has_new_ack = false; // clean
            lck.unlock();
            // call predicate
            predicate_lambda(mcs);
        }
        log_exit_func();
    }

    void WanAgentSender::shutdown_and_wait()
    {
        log_enter_func();
        is_shutdown.store(true);
        report_new_ack();        // TODO: clientGuess: to wake up all predicate_loop threads with a pusedo "new ack"
        predicate_thread.join(); // TODO: client
        log_exit_func();
    }

} // namespace wan_agent