#include "limen/app/harness.hpp"
#include "limen/app/exit_codes.hpp"
#include "limen/format.hpp"
#include "limen/pattern.hpp"
#include <format>
#include <iostream>
#include <sys/poll.h>



bool can_post(RunConfig &run_config, RunState &state)
{
    uint64_t outstanding_sends = state.posted - state.covered;
    bool has_work = run_config.is_client ? true : state.responses_owed>0;
    return (
        has_work && 
        state.posted < run_config.iterations && 
        (outstanding_sends < run_config.eff_pipeline) &&
        (run_config.inline_ok || state.covered+ run_config.eff_pipeline>state.posted)
        && (run_config.max_outstanding == 0 ||
            state.posted - state.recv_count < run_config.max_outstanding)
    );
}

int post_send(
    bool signaled,
    uint32_t seq,
    uint32_t num_slots,
    uint64_t buff_addr,
    ibv_qp* queue_pair,
    uint32_t message_size,
    uint32_t lkey,
    bool inline_enabled,
    bool fenced
)
{
    //  create the ibv_send_wr
    ibv_send_wr wr{};
    ibv_sge sge{};
    ibv_send_wr* bad = nullptr;

    //  for now each entry has a single SGE
    //  slot[i] has address &(buffer) + ([i]* [message_size])
    uint32_t slot = seq % num_slots;
    sge.addr = limen::slot_addr(buff_addr, slot, message_size);
    sge.length = message_size;
    sge.lkey = lkey;

    wr.num_sge = 1;
    wr.sg_list = &sge;
    wr.next = nullptr;
    wr.wr_id = seq | SEND_WRID_TAG;
    wr.opcode =  IBV_WR_SEND;
    wr.send_flags =   fenced? IBV_SEND_FENCE : 0;
    
    if (inline_enabled)
    {
        wr.send_flags |= IBV_SEND_INLINE;
    }
    if (signaled) 
    {
        wr.send_flags |= IBV_SEND_SIGNALED;
    }

    return ibv_post_send(queue_pair, &wr, &bad);
}


//  0 on success 1 on fail
int post_one(limen::Session &session, RunConfig &run_config, RunState &state)
{
    uint64_t send_slot_num = state.posted % run_config.send_slots;
    void* send_addr = reinterpret_cast<void*>(limen::slot_addr((uint64_t)(uintptr_t)session.send_mr()->addr, send_slot_num, run_config.message_size));
    if (run_config.verify_payload)
        limen::fill_pattern(send_addr, run_config.message_size, state.posted);
    bool signal_this_event =  (state.posted % run_config.eff_signal_every == 0 || (state.posted+1 == run_config.iterations));
    
    uint64_t len = run_config.response_size > 0 ? run_config.response_size : run_config.message_size;

    int rc = post_send(
        signal_this_event,
        state.posted,
        run_config.send_slots,
        (uint64_t)(uintptr_t)session.send_mr()->addr,
        session.qp(),
        (uint32_t)len,
        session.send_mr()->lkey,
        run_config.inline_ok,
        run_config.fenced
    );
    if (rc != 0)
    {
        fprintf(stderr,"post_one:post_send %s (%s)\n",strerrorname_np(rc),strerror(rc));
        return EXIT_VERB_ERROR;
    }
    state.posted++;
    if (run_config.is_client == false)
    {
        state.responses_owed--;
    }

    return 0;
}

//  returns 0 on successful WC, 1 if the WC is not successful, -1 on unexpected error 
int handle_wc(ibv_wc& wc, limen::Session &session, RunConfig &run_config, RunState &state)
{

    if (wc.status != IBV_WC_SUCCESS)
    {
        ibv_qp_init_attr qp_init_attr{};
        ibv_qp_attr      qp_attr{};
        //  if the status is not successful then WCs from this one onward
        //  are bad & have to be flushed accordingly
        std::cout << std::format("qp_num={:#08x}\n",session.qp()->qp_num);
        std::cout << "\tnote: opcode and byte_len are not valid on an error completion\n";
        ibv_query_qp(session.qp(), &qp_attr, IBV_QP_STATE, &qp_init_attr);
        std::cout << "qp_state_after_error: " << limen::qp_state_to_str(qp_attr.cur_qp_state)  << std::endl;
        if (state.first_error_status == IBV_WC_SUCCESS)
        {
            std::fprintf(stderr, "wc error: %s (%d) wr_id=%#lx\n", ibv_wc_status_str(wc.status), wc.status, wc.wr_id);
            state.first_error_status = wc.status;
        }
        return 1;
    }
    else
    {
        //  successful, increment counters
        if (wc.opcode == IBV_WC_SEND)
        {
            uint64_t send_seq_num = limen::Session::remove_tags(wc.wr_id);
            state.covered = send_seq_num+1;
            state.send_completions++;
        }
        else if(wc.opcode == IBV_WC_RECV)
        {
            //  verify the payload
            uint32_t recv_slot_num = limen::Session::remove_tags(wc.wr_id);
            void* recv_addr = reinterpret_cast<void*>(
                limen::slot_addr((uint64_t)(uintptr_t)session.recv_mr()->addr, recv_slot_num, run_config.message_size));
            if (run_config.verify_payload)
                if (limen::verify_pattern(recv_addr, wc.byte_len, state.recv_count) > 0) state.mismatches++;
            //  repost the recv
            int rc = session.repost_recv(recv_slot_num);
            if (rc !=0)
            {
                //  failed to allocate slot
                fprintf(stderr,"handle_wc:post_recv %s (%s)\n",strerrorname_np(rc),strerror(rc));
                return -1;
            }
            state.recv_count++;
            if (run_config.is_client == false)
            {
                run_config.response_size = wc.byte_len;
                state.responses_owed++;
            }
        }

    }
    return 0;
}


int drain(limen::Session &session, RunConfig &run_config, RunState &state)
{
    int handled = 0;

    //  poll->handle() to clear out what's present
    ibv_wc wc;
    while (ibv_poll_cq(session.cq(),1,&wc)>0) 
    {
        if (handle_wc(wc,session,run_config,state) <0) return -1;
        // std::cout << limen::wc_to_str(wc) << std::endl;
        handled++;
    }
    return handled;
}

//  handles reaping cqe in event mode
int reap_event(limen::Session &session, RunConfig &run_config, RunState &state)
{
    int handled = 0;
    ibv_wc wc{};

    //  poll->handle() to clear out what's present
    handled = drain(session,run_config,state);
    if (handled < 0) return -1;
    if (handled > 0) return handled;        // caller may be able to post now

    //  arm()
    if (session.req_notify_cq(0) !=0) return -1;

    //  poll()->handle() to get race polls
    if (!run_config.broken_arming_enabled)
    {
        int n = drain(session, run_config, state);
        if (n < 0) return -1;
        if (n > 0) { state.race_polls_hit++; return n; }
    }

    //poll on completion channel fd
    pollfd pfd{};
    pfd.fd = session.comp_channel_fd();
    pfd.events = POLLIN;
    int poll_result = poll(&pfd, 1, run_config.poll_timeout_ms);
    if (poll_result == 0)
    {
        state.timeout = true;
        return handled;
    } 
    else if (poll_result <0)
    {
        if (errno == EINTR) return handled;
        return -1;
    }

    //  get_cq_event()
    if (session.get_cq_event() != 0) return handled;
    state.events_received++;

    //  poll()->handle()
    int start_count = handled;
    while (ibv_poll_cq(session.cq(),1,&wc)>0) 
    {
        if (handle_wc(wc,session,run_config,state) <0) return -1;
        // std::cout << limen::wc_to_str(wc) << std::endl;
        handled++;
    }
    if (start_count == handled)
        state.empty_events++;   // no completions 

    //  ack()
    if (state.events_received - state.events_acked >= 64)
    {
        if (session.ack_cq_events(64)!=0)
            return -1;
        state.events_acked +=64;
    }

    return handled;   
}

//  handles reaping cqe in poll mode
int reap_poll(limen::Session &session, RunConfig &run_config, RunState &state)
{
    return  drain(session, run_config, state);
}

//  returns # of successful reaps or -1 on fail
int reap(limen::Session &session, RunConfig &run_config, RunState &state)
{
    return run_config.wc_reap_mode == reap_mode::EVENT ? 
        reap_event(session,run_config,state) : 
        reap_poll(session,run_config,state);
}