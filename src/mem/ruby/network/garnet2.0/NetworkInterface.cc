/*
 * Copyright (c) 2008 Princeton University
 * Copyright (c) 2016 Georgia Institute of Technology
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Niket Agarwal
 *          Tushar Krishna
 */


#include "mem/ruby/network/garnet2.0/NetworkInterface.hh"

#include <cassert>
#include <cmath>
#include <ctime>

#include "base/cast.hh"
#include "base/stl_helpers.hh"
#include "debug/RubyNetwork.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/network/garnet2.0/Credit.hh"
#include "mem/ruby/network/garnet2.0/flitBuffer.hh"
#include "mem/ruby/slicc_interface/Message.hh"

using namespace std;
using m5::stl_helpers::deletePointers;

NetworkInterface::NetworkInterface(const Params *p)
    : ClockedObject(p), Consumer(this), m_id(p->id),
      m_virtual_networks(p->virt_nets), m_vc_per_vnet(p->vcs_per_vnet),
      m_num_vcs(m_vc_per_vnet * m_virtual_networks),// number of vc
      m_deadlock_threshold(p->garnet_deadlock_threshold),
      vc_busy_counter(m_virtual_networks, 0)


{
    m_router_id = -1;
    m_vc_round_robin = 0;
    m_ni_out_vcs.resize(m_num_vcs);
    m_ni_out_vcs_enqueue_time.resize(m_num_vcs);
    outCreditQueue = new flitBuffer();

    // instantiating the NI flit buffers
    for (int i = 0; i < m_num_vcs; i++) {
        m_ni_out_vcs[i] = new flitBuffer();
        m_ni_out_vcs_enqueue_time[i] = Cycles(INFINITE_);
    }

    m_vc_allocator.resize(m_virtual_networks); // 1 allocator per vnet
    for (int i = 0; i < m_virtual_networks; i++) {
        m_vc_allocator[i] = 0;
    }

    m_stall_count.resize(m_virtual_networks);

    //task graph
    core_buffer_round_robin = 0;
}

void
NetworkInterface::init()
{
    for (int i = 0; i < m_num_vcs; i++) {
        m_out_vc_state.push_back(new OutVcState(i, m_net_ptr));
    }
    entrance_NI = m_net_ptr->get_entrance_NI();
    entrance_core = m_net_ptr->get_entrance_core();
    entrance_idx_in_NI = m_net_ptr->get_entrance_idx_in_NI();
    m_num_apps = m_net_ptr->get_m_num_application();

    if(m_id==entrance_NI){
        num_initial_thread = lookUpMap(m_core_id_thread, entrance_core);
        initial_task_thread_queue = new int[num_initial_thread];
        initial_task_busy_flag = new bool[num_initial_thread];
        remainad_initial_task_exec_time = new int[num_initial_thread];
        app_idx_in_initial_thread_queue = new int[num_initial_thread];
        // initial_app_ratio_token = new int[m_num_apps];
        // for (int i=0; i<m_num_apps;i++){
        //     initial_app_ratio_token[i] = 0;
        // }
        for (int i=0;i<num_initial_thread;i++){
            initial_task_thread_queue[i] = -1;
            initial_task_busy_flag[i] = false;
            remainad_initial_task_exec_time[i] = -1;
            app_idx_in_initial_thread_queue[i] = -1;
        }
    }
}

NetworkInterface::~NetworkInterface()
{
    deletePointers(m_out_vc_state);
    deletePointers(m_ni_out_vcs);
    delete outCreditQueue;
    delete outFlitQueue;

    //for the task parallelism release memory
    for (int i=0;i<m_num_cores;i++){
        delete [] task_in_thread_queue[i];
        delete [] remained_execution_time_in_thread[i];
        delete [] thread_busy_flag[i];
        delete [] task_to_exec_round_robin[i];
        delete [] app_idx_in_thread_queue[i];
    }
    delete [] task_in_thread_queue;
    delete [] remained_execution_time_in_thread;
    delete [] thread_busy_flag;
    delete [] task_to_exec_round_robin;
    delete [] app_exec_rr;
    delete [] app_idx_in_thread_queue;

    delete [] m_total_data_bits;

    if(m_id==entrance_NI){
        delete [] initial_task_thread_queue;
        delete [] initial_task_busy_flag;
        delete [] remainad_initial_task_exec_time;
        delete [] app_idx_in_initial_thread_queue;
        // delete [] initial_app_ratio_token;
    }
}

void
NetworkInterface::addInPort(NetworkLink *in_link,
                              CreditLink *credit_link)
{
    inNetLink = in_link;
    in_link->setLinkConsumer(this);
    outCreditLink = credit_link;
    credit_link->setSourceQueue(outCreditQueue);
}

void
NetworkInterface::addOutPort(NetworkLink *out_link,
                             CreditLink *credit_link,
                             SwitchID router_id)
{
    inCreditLink = credit_link;
    credit_link->setLinkConsumer(this);

    outNetLink = out_link;
    outFlitQueue = new flitBuffer();
    out_link->setSourceQueue(outFlitQueue);

    m_router_id = router_id;
}

void
NetworkInterface::addNode(vector<MessageBuffer *>& in,
                            vector<MessageBuffer *>& out)
{
    inNode_ptr = in;
    outNode_ptr = out;

    for (auto& it : in) {
        if (it != nullptr) {
            it->setConsumer(this);
        }
    }
}

void
NetworkInterface::dequeueCallback()
{
    // An output MessageBuffer has dequeued something this cycle and there
    // is now space to enqueue a stalled message. However, we cannot wake
    // on the same cycle as the dequeue. Schedule a wake at the soonest
    // possible time (next cycle).
    scheduleEventAbsolute(clockEdge(Cycles(1)));
}

void
NetworkInterface::incrementStats(flit *t_flit)
{
    int vnet = t_flit->get_vnet();
    // Latency
    m_net_ptr->increment_received_flits(vnet);
    Cycles network_delay =
        t_flit->get_dequeue_time() - t_flit->get_enqueue_time() - Cycles(1);
    Cycles src_queueing_delay = t_flit->get_src_delay();
    Cycles dest_queueing_delay = (curCycle() - t_flit->get_dequeue_time());
    Cycles queueing_delay = src_queueing_delay + dest_queueing_delay;

    m_net_ptr->increment_flit_network_latency(network_delay, vnet);
    m_net_ptr->increment_flit_queueing_latency(queueing_delay, vnet);

    if (t_flit->get_type() == TAIL_ || t_flit->get_type() == HEAD_TAIL_) {
        m_net_ptr->increment_received_packets(vnet);
        m_net_ptr->increment_packet_network_latency(network_delay, vnet);
        m_net_ptr->increment_packet_queueing_latency(queueing_delay, vnet);
    }

    // Hops
    m_net_ptr->increment_total_hops(t_flit->get_route().hops_traversed);
}
void
NetworkInterface::incrementStats(flit *t_flit, bool in_core)
{
    int vnet = t_flit->get_vnet();
    // Latency
    m_net_ptr->increment_received_flits(vnet);
    Cycles network_delay;
    if(!in_core){
        network_delay =
            t_flit->get_dequeue_time() - t_flit->get_enqueue_time() - Cycles(1);
    }else{
        network_delay =
        t_flit->get_dequeue_time() - t_flit->get_enqueue_time();
    }
    
    Cycles src_queueing_delay = t_flit->get_src_delay();
    Cycles dest_queueing_delay = (curCycle() - t_flit->get_dequeue_time());
    Cycles queueing_delay = src_queueing_delay + dest_queueing_delay;

    m_net_ptr->increment_flit_network_latency(network_delay, vnet);
    m_net_ptr->increment_flit_queueing_latency(queueing_delay, vnet);

    if (t_flit->get_type() == TAIL_ || t_flit->get_type() == HEAD_TAIL_) {
        m_net_ptr->increment_received_packets(vnet);
        m_net_ptr->increment_packet_network_latency(network_delay, vnet);
        m_net_ptr->increment_packet_queueing_latency(queueing_delay, vnet);
    }

    // Hops
    m_net_ptr->increment_total_hops(t_flit->get_route().hops_traversed);
}

/*
 * The NI wakeup checks whether there are any ready messages in the protocol
 * buffer. If yes, it picks that up, flitisizes it into a number of flits and
 * puts it into an output buffer and schedules the output link. On a wakeup
 * it also checks whether there are flits in the input link. If yes, it picks
 * them up and if the flit is a tail, the NI inserts the corresponding message
 * into the protocol buffer. It also checks for credits being sent by the
 * downstream router.
 */

void
NetworkInterface::wakeup()
{
    DPRINTF(RubyNetwork, "Network Interface %d connected to router %d "
            "woke up at time: %lld\n", m_id, m_router_id, curCycle());

    MsgPtr msg_ptr;
    Tick curTime = clockEdge();

    // Checking for messages coming from the protocol
    // can pick up a message/cycle for each virtual net
    if (!m_net_ptr->isTaskGraphEnabled()){
        for (int vnet = 0; vnet < inNode_ptr.size(); ++vnet) {
            MessageBuffer *b = inNode_ptr[vnet];
            if (b == nullptr) {
                continue;
            }

            if (b->isReady(curTime)) { // Is there a message waiting
                msg_ptr = b->peekMsgPtr();
                if (flitisizeMessage(msg_ptr, vnet)) {
                    b->dequeue(curTime);
                }
            }
        }
    } else {
        enqueueTaskInThreadQueue();
        task_execution();
        updateGeneratorBuffer();
        coreSendFlitsOut();
    }

    scheduleOutputLink();
    checkReschedule();

    // Check if there are flits stalling a virtual channel. Track if a
    // message is enqueued to restrict ejection to one message per cycle.
    bool messageEnqueuedThisCycle = checkStallQueue();

    /*********** Check the incoming flit link **********/
    if (inNetLink->isReady(curCycle())) {

        if (!m_net_ptr->isTaskGraphEnabled()){
            flit *t_flit = inNetLink->consumeLink();
            int vnet = t_flit->get_vnet();
            t_flit->set_dequeue_time(curCycle());

            // If a tail flit is received, enqueue into the protocol buffers if
            // space is available. Otherwise, exchange non-tail flits for credits.
            if (t_flit->get_type() == TAIL_ || t_flit->get_type() \
            == HEAD_TAIL_) {
                if (!messageEnqueuedThisCycle &&
                outNode_ptr[vnet]->areNSlotsAvailable(1, curTime)) {
                // Space is available. Enqueue to protocol buffer.
                outNode_ptr[vnet]->enqueue(t_flit->get_msg_ptr(), curTime,
                                            cyclesToTicks(Cycles(1)));

                // Simply send a credit back since we are not buffering
                // this flit in the NI
                sendCredit(t_flit, true);

                // Update stats and delete flit pointer
                incrementStats(t_flit);
                delete t_flit;
                } else {
                // No space available- Place tail flit in stall queue and set
                // up a callback for when protocol buffer is dequeued. Stat
                // update and flit pointer deletion will occur upon unstall.
                m_stall_queue.push_back(t_flit);
                m_stall_count[vnet]++;

                auto cb = std::bind(&NetworkInterface::dequeueCallback, this);
                outNode_ptr[vnet]->registerDequeueCallback(cb);
                }
            } else {
                // Non-tail flit. Send back a credit but not VC free signal.
                sendCredit(t_flit, false);

                // Update stats and delete flit pointer.
                incrementStats(t_flit);
                delete t_flit;
            }
        }
        else {
            flit *t_flit = inNetLink->consumeLink();
            int temp_task = t_flit->get_tg_info().dest_task;
            int temp_edge_id = t_flit->get_tg_info().edge_id;
            int app_idx = t_flit->get_tg_info().app_idx;
            int core_id = get_core_id_by_task_id(app_idx, temp_task);

            GraphTask &dest_task = get_task_by_task_id(core_id, app_idx, temp_task);
            GraphEdge &dest_edge = dest_task.get_incoming_edge_by_eid(temp_edge_id);

            t_flit->set_dequeue_time(curCycle());

            if (t_flit->get_type() == TAIL_ || t_flit->get_type() == HEAD_TAIL_)
            {
                //received a pkt
                dest_edge.record_pkt(t_flit, curCycle());   //operate in this task's in edge(in mem write)
                /*
                DPRINTF(TaskGraph, " NI %d received the tail flit \
                from the NI %d \n", m_id, t_flit->get_route().src_ni);
                */
                sendCredit(t_flit, true);
                // Update stats and delete flit pointer
                incrementStats(t_flit);
                delete t_flit;
            }
            else
            {
                sendCredit(t_flit, false);
                // Update stats and delete flit pointer.
                incrementStats(t_flit);
                delete t_flit;
            }

            /*
            if (input_buffer[core_idx].size() == input_buffer_size[core_idx] ){
                printf("[%3d] Input Buffer Full !\n", core_id);
            } else {
                flit *t_flit = inNetLink->consumeLink();
                input_buffer[core_idx].push_back(t_flit);
                assert(input_buffer[core_idx].capacity()==input_buffer_size[core_idx]);
            }
            */
        }
    }

    /****************** Check the incoming credit link *******/

    if (inCreditLink->isReady(curCycle())) {
        Credit *t_credit = (Credit*) inCreditLink->consumeLink();
        m_out_vc_state[t_credit->get_vc()]->increment_credit();
        if (t_credit->is_free_signal()) {
            m_out_vc_state[t_credit->get_vc()]->setState(IDLE_, curCycle());
        }
        delete t_credit;
    }


    // It is possible to enqueue multiple outgoing credit flits if a message
    // was unstalled in the same cycle as a new message arrives. In this
    // case, we should schedule another wakeup to ensure the credit is sent
    // back.
    if (outCreditQueue->getSize() > 0) {
        outCreditLink->scheduleEventAbsolute(clockEdge(Cycles(1)));
    }

    /****************** Core get the flit from input buffer *******/
    /*
    for (int i=0; i<m_num_cores; i++) {

        if (input_buffer[i].size()==0){
            continue;
        }

        flit* t_flit = input_buffer[i].front();
        int temp_src = t_flit->get_tg_info().src_task;
        int temp_edge_id = t_flit->get_tg_info().edge_id;
        int temp_task = t_flit->get_tg_info().dest_task;
        int app_idx = t_flit->get_tg_info().app_idx;
        int core_id = get_core_id_by_task_id(app_idx, temp_task);
        int core_idx = lookUpMap(m_core_id_index, core_id);
        assert(core_idx == i);

        GraphTask &dest_task = get_task_by_task_id(core_id, app_idx, temp_task);
        GraphEdge &dest_edge = dest_task.get_incoming_edge_by_eid(temp_edge_id);
        assert(dest_edge.get_src_task_id() == temp_src);

        // if receive token more than 10, do nothing
        if (dest_edge.get_num_incoming_token()>not_used_token){
            //printf("[%3d] cannot get the new flit and the [%3d] tokens not used\n", core_id, dest_edge.get_num_incoming_token());
        } else {
            t_flit->set_dequeue_time(curCycle());

            if (t_flit->get_type() == TAIL_ || t_flit->get_type() == HEAD_TAIL_) {
                //received a pkt
                dest_edge.record_pkt(t_flit, curCycle());
                DPRINTF(TaskGraph, " NI %d received the tail flit from the NI %d \n", m_id, t_flit->get_route().src_ni);
                sendCredit(t_flit, true);
                // Update stats and delete flit pointer
                incrementStats(t_flit);
                delete t_flit;
            } else {
                sendCredit(t_flit, false);
                // Update stats and delete flit pointer.
                incrementStats(t_flit);
                delete t_flit;
            }
            input_buffer[i].erase(input_buffer[i].begin());
        }
    }*/
}

void
NetworkInterface::sendCredit(flit *t_flit, bool is_free)
{
    Credit *credit_flit = new Credit(t_flit->get_vc(), is_free, curCycle());
    outCreditQueue->insert(credit_flit);
}

bool
NetworkInterface::checkStallQueue()
{
    bool messageEnqueuedThisCycle = false;
    Tick curTime = clockEdge();

    if (!m_stall_queue.empty()) {
        for (auto stallIter = m_stall_queue.begin();
            stallIter != m_stall_queue.end(); ) {
            flit *stallFlit = *stallIter;
            int vnet = stallFlit->get_vnet();

            // If we can now eject to the protocol buffer, send back credits
            if (outNode_ptr[vnet]->areNSlotsAvailable(1, curTime)) {
                outNode_ptr[vnet]->enqueue(stallFlit->get_msg_ptr(), curTime,
                                        cyclesToTicks(Cycles(1)));

                // Send back a credit with free signal now that the VC is no
                // longer stalled.
                sendCredit(stallFlit, true);

                // Update Stats
                incrementStats(stallFlit);

                // Flit can now safely be deleted and removed from stall queue
                delete stallFlit;
                m_stall_queue.erase(stallIter);
                m_stall_count[vnet]--;

                // If there are no more stalled messages for this vnet, the
                // callback on it's MessageBuffer is not needed.
                if (m_stall_count[vnet] == 0)
                    outNode_ptr[vnet]->unregisterDequeueCallback();

                messageEnqueuedThisCycle = true;
                break;
            } else {
                ++stallIter;
            }
        }
    }

    return messageEnqueuedThisCycle;
}

// Embed the protocol message into flits
bool
NetworkInterface::flitisizeMessage(MsgPtr msg_ptr, int vnet)
{
    Message *net_msg_ptr = msg_ptr.get();
    NetDest net_msg_dest = net_msg_ptr->getDestination();

    // gets all the destinations associated with this message.
    vector<NodeID> dest_nodes = net_msg_dest.getAllDest();

    // Number of flits is dependent on the link bandwidth available.
    // This is expressed in terms of bytes/cycle or the flit size
    int num_flits = (int) ceil((double) m_net_ptr->MessageSizeType_to_int(
        net_msg_ptr->getMessageSize())/m_net_ptr->getNiFlitSize());

    // loop to convert all multicast messages into unicast messages
    for (int ctr = 0; ctr < dest_nodes.size(); ctr++) {

        // this will return a free output virtual channel
        int vc = calculateVC(vnet);

        if (vc == -1) {
            return false ;
        }
        MsgPtr new_msg_ptr = msg_ptr->clone();
        NodeID destID = dest_nodes[ctr];

        Message *new_net_msg_ptr = new_msg_ptr.get();
        if (dest_nodes.size() > 1) {
            NetDest personal_dest;
            for (int m = 0; m < (int) MachineType_NUM; m++) {
                if ((destID >= MachineType_base_number((MachineType) m)) &&
                    destID < MachineType_base_number((MachineType) (m+1))) {
                    // calculating the NetDest associated with this destID
                    personal_dest.clear();
                    //next to add is MachineID constructor
                    personal_dest.add((MachineID) {(MachineType) m, (destID -
                        MachineType_base_number((MachineType) m))});
                    new_net_msg_ptr->getDestination() = personal_dest;
                    break;
                }
            }
            net_msg_dest.removeNetDest(personal_dest);
            // removing the destination from the original message to reflect
            // that a message with this particular destination has been
            // flitisized and an output vc is acquired
            net_msg_ptr->getDestination().removeNetDest(personal_dest);
        }

        // Embed Route into the flits
        // NetDest format is used by the routing table
        // Custom routing algorithms just need destID
        RouteInfo route;
        route.vnet = vnet;
        route.net_dest = new_net_msg_ptr->getDestination();
        route.src_ni = m_id;
        route.src_router = m_router_id;
        route.dest_ni = destID;
        route.dest_router = m_net_ptr->get_router_id(destID);
        route.vc_choice = (route.dest_router >= route.src_router);

        // initialize hops_traversed to -1
        // so that the first router increments it to 0
        route.hops_traversed = -1;

        m_net_ptr->increment_injected_packets(vnet);
        for (int i = 0; i < num_flits; i++) {
            m_net_ptr->increment_injected_flits(vnet);
            flit *fl = new flit(i, vc, vnet, route, num_flits, new_msg_ptr,
                curCycle());

            fl->set_src_delay(curCycle() - ticksToCycles(msg_ptr->getTime()));
            m_ni_out_vcs[vc]->insert(fl);
        }

        m_ni_out_vcs_enqueue_time[vc] = curCycle();
        m_out_vc_state[vc]->setState(ACTIVE_, curCycle());
    }
    return true ;
}

// Looking for a free output vc
int
NetworkInterface::calculateVC(int vnet)
{
    for (int i = 0; i < m_vc_per_vnet; i++) {
        int delta = m_vc_allocator[vnet];
        m_vc_allocator[vnet]++;
        if (m_vc_allocator[vnet] == m_vc_per_vnet)
            m_vc_allocator[vnet] = 0;

        if (m_out_vc_state[(vnet*m_vc_per_vnet) + delta]->isInState(
                    IDLE_, curCycle())) {
            vc_busy_counter[vnet] = 0;
            return ((vnet*m_vc_per_vnet) + delta);
        }
    }

    vc_busy_counter[vnet] += 1;
    // Commented by Wang Jing, remove the deadlock check

    panic_if(vc_busy_counter[vnet] > m_deadlock_threshold,
        "%s: Possible network deadlock in vnet: %d at time: %llu \n",
        name(), vnet, curTick());


    return -1;
}

//
int
NetworkInterface::getNumRemainedIdleVC(int vnet)
{
    int remainded_num_vc = 0;
    for (int i = 0; i < m_vc_per_vnet; i++) {
        if(m_out_vc_state[(vnet*m_vc_per_vnet) + i]->isInState(IDLE_, curCycle()))
            remainded_num_vc++;
    }
    return remainded_num_vc;
}

/** This function looks at the NI buffers
 *  if some buffer has flits which are ready to traverse the link in the next
 *  cycle, and the downstream output vc associated with this flit has buffers
 *  left, the link is scheduled for the next cycle
 */

void
NetworkInterface::scheduleOutputLink()
{
    int vc = m_vc_round_robin;

    for (int i = 0; i < m_num_vcs; i++) {
        vc++;
        if (vc == m_num_vcs)
            vc = 0;

        // model buffer backpressure
        if (m_ni_out_vcs[vc]->isReady(curCycle()) &&
            m_out_vc_state[vc]->has_credit()) {

            bool is_candidate_vc = true;
            int t_vnet = get_vnet(vc);
            int vc_base = t_vnet * m_vc_per_vnet;

            //In Garnet_standalone, the vnet2 isVNetOrdered=false
            if (m_net_ptr->isVNetOrdered(t_vnet)) {
                for (int vc_offset = 0; vc_offset < m_vc_per_vnet;
                    vc_offset++) {
                    int t_vc = vc_base + vc_offset;
                    if (m_ni_out_vcs[t_vc]->isReady(curCycle())) {
                        if (m_ni_out_vcs_enqueue_time[t_vc] <
                            m_ni_out_vcs_enqueue_time[vc]) {
                            is_candidate_vc = false;
                            break;
                        }
                    }
                }
            }
            if (!is_candidate_vc)
                continue;

            m_vc_round_robin = vc;

            m_out_vc_state[vc]->decrement_credit();
            // Just removing the flit
            flit *t_flit = m_ni_out_vcs[vc]->getTopFlit();
            t_flit->set_time(curCycle() + Cycles(1));
            outFlitQueue->insert(t_flit);
            // schedule the out link
            outNetLink->scheduleEventAbsolute(clockEdge(Cycles(1)));

            if (t_flit->get_type() == TAIL_ ||
                t_flit->get_type() == HEAD_TAIL_) {
                m_ni_out_vcs_enqueue_time[vc] = Cycles(INFINITE_);
            }
            return;
        }
    }
}

int
NetworkInterface::get_vnet(int vc)
{
    for (int i = 0; i < m_virtual_networks; i++) {
        if (vc >= (i*m_vc_per_vnet) && vc < ((i+1)*m_vc_per_vnet)) {
            return i;
        }
    }
    fatal("Could not determine vc");
}


// Wakeup the NI in the next cycle if there are waiting
// messages in the protocol buffer, or waiting flits in the
// output VC buffer
void
NetworkInterface::checkReschedule()
{
    for (const auto& it : inNode_ptr) {
        if (it == nullptr) {
            continue;
        }

        while (it->isReady(clockEdge())) { // Is there a message waiting
            scheduleEvent(Cycles(1));
            return;
        }
    }

    for (int vc = 0; vc < m_num_vcs; vc++) {
        if (m_ni_out_vcs[vc]->isReady(curCycle() + Cycles(1))) {
            scheduleEvent(Cycles(1));
            return;
        }
    }

    //for task graph, every cycle wake up the NI
    scheduleEvent(Cycles(1));
}

void
NetworkInterface::print(std::ostream& out) const
{
    out << "[Network Interface]";
}

uint32_t
NetworkInterface::functionalWrite(Packet *pkt)
{
    uint32_t num_functional_writes = 0;
    for (unsigned int i  = 0; i < m_num_vcs; ++i) {
        num_functional_writes += m_ni_out_vcs[i]->functionalWrite(pkt);
    }

    num_functional_writes += outFlitQueue->functionalWrite(pkt);
    return num_functional_writes;
}

NetworkInterface *
GarnetNetworkInterfaceParams::create()
{
    return new NetworkInterface(this);
}

//for task graph traffic
int
NetworkInterface::add_task(int app_idx, GraphTask &t, bool is_head_task){
    int core_id = t.get_proc_id();
    int idx = lookUpMap(m_core_id_index, core_id);
    if (is_head_task)
        head_task_list[idx][app_idx].push_back(t);
    else
        task_list[idx][app_idx].push_back(t);
    return 0;
}

int
NetworkInterface::sort_task_list()
{
    for (unsigned i=0;i<task_list.size();i++)
        for (unsigned j=0;j<task_list[i].size();j++)
            sort( task_list[i][j].begin(), task_list[i][j].end(), compare);
    return 0;
}

void
NetworkInterface::initializeTaskIdList(){
    //record the task id in the task_id_list
    for (unsigned i=0;i<task_list.size();i++){
        for (unsigned j=0;j<task_list[i].size();j++){
            task_id_list[i][j].resize(task_list[i][j].size());
            for(unsigned k=0;k<task_list[i][j].size();k++){
                task_id_list[i][j][k] = task_list[i][j][k].get_id();
            }
        }        
    }
}

int
NetworkInterface::get_task_offset_by_task_id(int core_id, int app_idx, int tid)
{
    int task_list_idx = lookUpMap(m_core_id_index, core_id);
    for (unsigned int i=0; i<task_list[task_list_idx][app_idx].size(); i++)
        {
                GraphTask &e = task_list[task_list_idx][app_idx].at(i) ;
                if (e.get_id() == tid)
                        return i;
        }
    fatal(" Error in finding task offset by task id ! ");
}

GraphTask&
NetworkInterface::get_task_by_task_id(int core_id, int app_idx, int tid)
{
    int task_list_idx = lookUpMap(m_core_id_index, core_id);
    for (unsigned int i=0; i<task_list[task_list_idx][app_idx].size(); i++)
        {
                GraphTask &e = task_list[task_list_idx][app_idx].at(i) ;
                if (e.get_id() == tid)
                        return e;
        }
    fatal(" Error in finding task by task id ! ");
}

int
NetworkInterface::get_core_id_by_task_id(int app_idx, int tid){
    for (int i=0;i<m_num_cores;i++){
        for (unsigned int j=0;j<task_list[i][app_idx].size();j++){
            GraphTask &t = task_list[i][app_idx].at(j);
            if (t.get_id() == tid){
                int core_id = lookUpMap(m_index_core_id, i);
                assert(t.get_proc_id() == core_id);
                return core_id;
            }
        }
    }
    fatal("Error in getting core id by task id!");
}

int
NetworkInterface::get_core_id_by_index(int i){
    if (i<m_num_cores){
        int core_id = lookUpMap(m_index_core_id, i);
        return core_id;
    }else
        fatal("Core Index out of range !");
}

string
NetworkInterface::get_core_name_by_index(int i){
    if (i<m_num_cores){
        int core_id = lookUpMap(m_index_core_id, i);
        return m_core_id_name[core_id];
    }else
        fatal("Core Index out of range !");
}

int
NetworkInterface::lookUpMap(std::map<int, int> m, int idx){
    std::map<int, int>::iterator iter = m.find(idx);
    if (iter != m.end())
    //find the key
        return m[idx];
    else
        fatal("Error in finding key in map !");
}

void
NetworkInterface::enqueueTaskInThreadQueue()
{
    assert(task_list.size() == m_num_cores);
    //for debug, print info
    int test_core = -1;
    int test_task = -1;
    for (int i=0;i<m_num_cores;i++){
        //core information
        int current_core_id = lookUpMap(m_index_core_id, i);
        int num_threads = lookUpMap(m_core_id_thread, current_core_id);
        //if busy, jump to next core
        int idle_idx;
        for (idle_idx=0;idle_idx<num_threads;idle_idx++){
            if (!thread_busy_flag[i][idle_idx])
                break;
        }
        if (idle_idx == num_threads)
            continue;//break;

        bool isAllThreadRuning = false;

        //for multi-app
        //int app_rr_offset = 0;
        for (int kk=0; kk<m_num_apps;kk++){
            int app_idx = (kk+app_exec_rr[i]) % m_num_apps;
            if (task_list[i][app_idx].size()<=0)
                continue;
            /*
            if (current_core_id==-1){
                printf("Cycle <<%lu>> rr_idx%3d\n",\
                    u_int64_t(curCycle()),task_to_exec_round_robin[i][app_idx]);
            }
            */
            int round_robin_offset = 0;

            //for (unsigned int j=0;j<temp_task_id_list.size();j++){
            for (unsigned int ii=0;ii<task_list[i][app_idx].size();ii++){
                int j = (ii + task_to_exec_round_robin[i][app_idx]) % task_list[i][app_idx].size();
                //task round robin refers the task offset which the previous is
                //execute, so when the task execute in the following process, the
                //index would add 1.

                GraphTask &c_task = task_list[i][app_idx][j];
                assert(&(get_task_by_offset(current_core_id, app_idx, j))==&(task_list[i][app_idx][j]) && \
                    &(task_list[i][app_idx][j]) == &(c_task));

                if (current_core_id==test_core)
                    printf("Process Task %3d!\n", c_task.get_id());

                if (c_task.get_id() == 0){
                    assert(m_id==entrance_NI);
                    round_robin_offset += 1;
                    continue;
                }

                //check the state of task
                // if (c_task.get_completed_times()>=c_task.get_required_times())
                //     continue;
                // else if (c_task.get_c_e_times()>=c_task.get_required_times()){
                //    assert(c_task.get_c_e_times()>=c_task.get_completed_times());
                //     continue;
                // }

                //check whether the thread queue busy
                int not_busy_idx;
                for (not_busy_idx=0;not_busy_idx<num_threads;not_busy_idx++){
                    if (!thread_busy_flag[i][not_busy_idx])
                        break;
                }
                //if all queue is busy, break to next core
                if (not_busy_idx == num_threads){
                    if (current_core_id==test_core)
                        printf("Task %3d not execute because of all queue are \
                        busy!\nThe core is executing %3d\n", c_task.get_id(), \
                        task_in_thread_queue[i][0]);

                    //for jump out of double loop
                    isAllThreadRuning = true;
                    break;
                }
                
                /****************** Check the task state ******************/
                //check the out_edge out_memory
                int jj;
                for (jj=0;jj<c_task.get_size_of_outgoing_edge_list();jj++){
                    GraphEdge &temp_edge = c_task.get_outgoing_edge_by_offset(jj);
                    if (temp_edge.get_out_memory_remained() <= 0){
                        break;
                    }
                    //assert(temp_edge.get_out_memory_remained() > 0);
                }
                if (jj < c_task.get_size_of_outgoing_edge_list())
                    continue;

                //the starting task without dependency on other tasks
                if (c_task.get_size_of_incoming_edge_list() == 0){
                    //add task to thread queue
    /*
                    if (current_core_id==0){
                            printf("Core %d task %d start executing\n", \
                            current_core_id, c_task.get_id());
                            printf("Task in executing list: ");
                            for (unsigned iii=0;iii<num_threads;iii++){
                                if (thread_busy_flag[i][iii])
                                    printf("Thread %d busy\t%d\t", iii ,\
                                        task_in_thread_queue[i][iii]);
                            }
                            printf("\n");
                    }
    */

                    if (m_net_ptr->IsPrintTaskExecuInfo())
                        *(m_net_ptr->task_start_time_vs_id->stream())<<\
                        u_int64_t(curCycle())<<"\t"<<current_core_id<<\
                        "\t"<<c_task.get_id()<<"\n";

                    int task_offset = get_task_offset_by_task_id(current_core_id, app_idx,c_task.get_id());
                    if (task_offset==task_to_exec_round_robin[i][app_idx]+round_robin_offset)
                        round_robin_offset += 1;

                    c_task.add_c_e_times();
                    task_in_thread_queue[i][not_busy_idx] = c_task.get_id();
                    app_idx_in_thread_queue[i][not_busy_idx] = app_idx;
                    thread_busy_flag[i][not_busy_idx] = true;
                    assert(remained_execution_time_in_thread[i][not_busy_idx] == -1);

                    int execution_time = c_task.get_random_execution_time();
                    m_net_ptr->add_execution_time_to_total(execution_time);
                    remained_execution_time_in_thread[i][not_busy_idx] = execution_time;
                    c_task.record_execution_time(curCycle(), curCycle()+execution_time);

                    if(c_task.get_c_e_times()<=c_task.get_required_times()){
                        m_net_ptr->update_start_end_time(app_idx, c_task.get_c_e_times()-1, curCycle(), curCycle()+execution_time);
                    }

                    //if the task have no in edges, we assume it can execute at the Cycle 0.
                    c_task.set_all_tokens_received_time(curCycle());

                    for (int k=0;k<c_task.get_size_of_outgoing_edge_list();k++){

                        GraphEdge &temp_edge = c_task.get_outgoing_edge_by_offset(k);
                        assert(temp_edge.update_out_memory_write_pointer());

                        int dest_proc_id = temp_edge.get_dst_proc_id();
                        if (dest_proc_id == c_task.get_proc_id()){
                            assert(dest_proc_id == current_core_id);
                        }

                        double token_size = temp_edge.get_random_token_size();
                        int num_flits = ceil(token_size / (m_net_ptr->getNiFlitSize() * 8));
                        m_total_data_bits[i] += token_size;

                        enqueueFlitsGeneratorBuffer(temp_edge, num_flits, execution_time);

                        temp_edge.generate_new_token();
                    }
                }// the task that depends on other tasks
                else {
                    int k;
                    for (k=0;k<c_task.get_size_of_incoming_edge_list();k++){
                        GraphEdge &temp_edge=c_task.get_incoming_edge_by_offset(k);
                        if (temp_edge.get_num_incoming_token()>0)
                            continue;
                        else
                            break;
                    }
                    ////the requirement is not satisfied
                    if (k < c_task.get_size_of_incoming_edge_list()){
                        if (current_core_id==test_core)
                            printf("Task %3d requirement is not satisfied\n", c_task.get_id());
                        continue;
                    }
                    // cout << m_id << "\t" << c_task.get_proc_id() << "\t" << c_task.get_id() << "\t" << curCycle() << endl;//////////////////////////////////////////////
                    if (current_core_id==test_core)
                            printf("Task %3d start\n", c_task.get_id());

                    for (k=0;k<c_task.get_size_of_incoming_edge_list();k++){

                        GraphEdge &temp_edge = c_task.get_incoming_edge_by_offset(k);
                        temp_edge.consume_token();
                        assert(temp_edge.update_in_memory_read_pointer());//operate in this task's in edge
                        int src_proc_id = temp_edge.get_src_proc_id();
                        int src_task_id = temp_edge.get_src_task_id();
                        int edge_id = temp_edge.get_id();

                        // if (src_proc_id == c_task.get_proc_id()){
                        //     assert(src_proc_id == current_core_id);
                        //     int src_task_id = temp_edge.get_src_task_id();
                        //     int edge_id = temp_edge.get_id();
                            // GraphTask &src_task = get_task_by_task_id(current_core_id, app_idx, src_task_id);
                            // GraphEdge &out_edge = src_task.get_outgoing_edge_by_eid(edge_id);
                            // assert(&out_edge == &temp_edge);
                            // assert(out_edge.update_out_memory_read_pointer());  //operate in last task's out edge
                            // m_net_ptr->update_in_memory_info(src_proc_id, app_idx, src_task_id, edge_id);
                        // } 
                        // else {
                            m_net_ptr->update_in_memory_info(src_proc_id, app_idx, src_task_id, edge_id);   //operate in last task's out edge(in mem read)//////////////////////////////
                        // }
                    }

                    /*if (current_core_id==4){
                            printf("Core %d task %d start executing\n", \
                            current_core_id, c_task.get_id());
                            printf("Task in executing list: ");
                            for (unsigned iii=0;iii<num_threads;iii++){
                                if (thread_busy_flag[i][iii])
                                    printf("%d\t", task_in_thread_queue[i][iii]);
                            }
                            printf("\n");
                    }*/

                    if (c_task.get_id()==test_task)
                        printf("Task %3d start executing %3d times\n", \
                            c_task.get_id(), c_task.get_c_e_times());

                    if (m_net_ptr->IsPrintTaskExecuInfo())
                        *(m_net_ptr->task_start_time_vs_id->stream())<<\
                            u_int64_t(curCycle())<<"\t"<<current_core_id<<\
                            "\t"<<c_task.get_id()<<"\n";
                    //if the current task is the first task, move
                    int task_offset = get_task_offset_by_task_id(current_core_id, app_idx,c_task.get_id());
                    if (task_offset==task_to_exec_round_robin[i][app_idx]+round_robin_offset)
                        round_robin_offset += 1;

                    c_task.add_c_e_times();
                    task_in_thread_queue[i][not_busy_idx] = c_task.get_id();
                    app_idx_in_thread_queue[i][not_busy_idx] = app_idx;
                    thread_busy_flag[i][not_busy_idx] = true;
                    assert(remained_execution_time_in_thread[i][not_busy_idx] == -1);

                    int execution_time = c_task.get_random_execution_time();
                    m_net_ptr->add_execution_time_to_total(execution_time);
                    remained_execution_time_in_thread[i][not_busy_idx] = execution_time;
                    c_task.record_execution_time(curCycle(), curCycle()+execution_time);
                    
                    if(c_task.get_c_e_times()<=c_task.get_required_times()){
                        m_net_ptr->update_start_end_time(app_idx, c_task.get_c_e_times()-1, curCycle(), curCycle()+execution_time);
                        
                    }

                    /*if (current_core_id==4){
                        printf("Cycle [ %lu ] Task rr ========> %3d\n", u_int64_t(curCycle()),task_to_exec_round_robin[i][app_idx]);
                        for (unsigned int iii=0;iii<task_list[i][app_idx].size();iii++){
                            GraphTask &t = task_list[i][app_idx][iii];
                            printf("Task %3d completed execute %3d times, ", t.get_id(), t.get_completed_times());
                            int min_token = INT16_MAX;
                            for (int l=0;l<t.get_size_of_incoming_edge_list();l++){
                                GraphEdge &temp_edge = t.get_incoming_edge_by_offset(l);
                                min_token = (min_token < temp_edge.get_total_incoming_token())?min_token:temp_edge.get_total_incoming_token();
                            }
                            printf("have received incoming token [ %3d ]\n", min_token);
                        }
                        printf("\n\n");
                    }*/

                    //For the task has in edges, compare the receive token time of
                    //the iteration and choose the max cycle.
                    int get_all_tokens_time = 0;
                    for (int ii=0;ii<c_task.get_size_of_incoming_edge_list();ii++){
                        
                        GraphEdge &temp_edge = c_task.get_incoming_edge_by_offset(ii);
                        int edge_get_token_time = temp_edge.get_token_received_time();
                        if (edge_get_token_time > get_all_tokens_time)
                            get_all_tokens_time = edge_get_token_time;
                        
                    }
                    c_task.set_all_tokens_received_time(get_all_tokens_time);
                    /*
                    printf("Core %3d Task %3d Waiting time %3d\n",\
                        current_core_id, c_task.get_id(),\
                        c_task.get_task_waiting_time(token_idx));
                    */
                    for (int k=0;k<c_task.get_size_of_outgoing_edge_list();k++){
                        GraphEdge &temp_edge = c_task.get_outgoing_edge_by_offset(k);
                        assert(temp_edge.update_out_memory_write_pointer());

                        int dest_proc_id = temp_edge.get_dst_proc_id();
                        if (dest_proc_id == c_task.get_proc_id()){
                            assert(dest_proc_id == current_core_id);
                            // int dest_task_id = temp_edge.get_dst_task_id();
                            // int edge_id = temp_edge.get_id();
                            // GraphTask &dst_task = get_task_by_task_id(dest_proc_id, app_idx, dest_task_id);
                            // GraphEdge &in_edge = dst_task.get_incoming_edge_by_eid(edge_id);
                            // assert(in_edge.update_in_memory_write_pointer());
                            // cout << "NI: " << m_id << "    core: " << dest_proc_id << "    two task in same core\n";
                        }

                        double token_size = temp_edge.get_random_token_size();
                        int num_flits = ceil(token_size / (m_net_ptr->getNiFlitSize() * 8));
                        m_total_data_bits[i] += token_size;

                        enqueueFlitsGeneratorBuffer(temp_edge, num_flits, execution_time);
                        temp_edge.generate_new_token();
                    }
                }
            }
            task_to_exec_round_robin[i][app_idx] = (task_to_exec_round_robin[i][app_idx] + round_robin_offset) % task_list[i][app_idx].size();
            
            // all thread runing, break the loop and jump to next core!
            if(isAllThreadRuning)
                break;

        }
        app_exec_rr[i] = (app_exec_rr[i] + 1) % m_num_apps;
    }

    if (m_id==entrance_NI){
        //if all token are 0; reset them
        int pp;
        for (pp = 0; pp < m_num_apps; pp++){
            if(initial_app_ratio_token[pp] > 0){
                break;
            }
        }
        if(pp == m_num_apps){
            reset_initial_app_ratio_token();
        }

        for (int kk=0; kk<m_num_apps;kk++){
            int app_idx = (kk+app_exec_rr[entrance_idx_in_NI]) % m_num_apps;
            if(initial_app_ratio_token[app_idx] <= 0){
                continue;
            }
        // GraphTask &c_task = task_list[entrance_idx_in_NI][app_idx][0];
        // assert(c_task.get_id()==0);

            int not_busy_idx;
            for (not_busy_idx = 0; not_busy_idx < num_initial_thread; not_busy_idx++)
            {
                if (!initial_task_busy_flag[not_busy_idx])
                    break;
            }

            if (not_busy_idx == num_initial_thread || m_net_ptr->back_pressure(entrance_NI)){
                break;
            } else {
                GraphTask &c_task = task_list[entrance_idx_in_NI][app_idx][0];
                assert(c_task.get_id()==0);
                //check the out_edge out_memory
                int jj;
                for (jj = 0; jj < c_task.get_size_of_outgoing_edge_list(); jj++)
                {
                    GraphEdge &temp_edge = c_task.get_outgoing_edge_by_offset(jj);
                    if (temp_edge.get_out_memory_remained() <= 0)
                    {
                        break;
                    }
                }
                if (jj < c_task.get_size_of_outgoing_edge_list()){
                    continue;
                } else {
                    assert(c_task.get_size_of_incoming_edge_list() == 0);
                    initial_app_ratio_token[app_idx]--; //consume token to reach certain ratio
                    c_task.add_c_e_times();
                    initial_task_thread_queue[not_busy_idx] = c_task.get_id();
                    initial_task_busy_flag[not_busy_idx] = true;
                    app_idx_in_initial_thread_queue[not_busy_idx] = app_idx;
                    int execution_time = c_task.get_random_execution_time();
                    remainad_initial_task_exec_time[not_busy_idx] = execution_time;
                    c_task.record_execution_time(curCycle(), curCycle()+execution_time);

                    if (c_task.get_c_e_times()<=c_task.get_required_times()){
                        m_net_ptr->update_start_end_time(app_idx, c_task.get_c_e_times()-1, curCycle(), curCycle()+execution_time);
                    }
                        

                    c_task.set_all_tokens_received_time(0);

                    for (int k=0;k<c_task.get_size_of_outgoing_edge_list();k++){

                        GraphEdge &temp_edge = c_task.get_outgoing_edge_by_offset(k);
                        assert(temp_edge.update_out_memory_write_pointer());
                        int dest_proc_id = temp_edge.get_dst_proc_id();
                        if (dest_proc_id == c_task.get_proc_id()){
                            assert(dest_proc_id == entrance_core);
                        }
                        double token_size = temp_edge.get_random_token_size();
                        int num_flits = ceil(token_size / (m_net_ptr->getNiFlitSize() * 8));

                        enqueueFlitsGeneratorBuffer(temp_edge, num_flits, execution_time);
                        temp_edge.generate_new_token();
                    }
                }
            }
        }
        app_exec_rr[entrance_idx_in_NI] = (app_exec_rr[entrance_idx_in_NI] + 1) % m_num_apps;
    }

}

void
NetworkInterface::task_execution()
{
    for (int i=0;i<m_num_cores;i++){
        int current_core_id = lookUpMap(m_index_core_id, i);
        int num_threads = lookUpMap(m_core_id_thread, current_core_id);
        for (int j=0;j<num_threads;j++){
            if (!thread_busy_flag[i][j]){
                assert(remained_execution_time_in_thread[i][j] == -1);
                continue;
            }else{
                remained_execution_time_in_thread[i][j]--;
                if (remained_execution_time_in_thread[i][j]<=0){

                    int c_task_id = task_in_thread_queue[i][j];
                    int app_idx = app_idx_in_thread_queue[i][j];
                    GraphTask &c_task = get_task_by_task_id(current_core_id, app_idx, c_task_id);
                    c_task.add_completed_times();
                    //for output dete delay
                    if(c_task.get_completed_times()<=c_task.get_required_times())
                        m_net_ptr->add_num_completed_tasks(app_idx, c_task.get_completed_times());
                    //reset the thread queue
                    remained_execution_time_in_thread[i][j] = -1;
                    thread_busy_flag[i][j] = false;
                    task_in_thread_queue[i][j] = -1;
                    app_idx_in_thread_queue[i][j] = -1;
                }
            }
        }
    }

    if (m_id==entrance_NI){
        for (int i=0;i<num_initial_thread;i++){
            if (!initial_task_busy_flag[i]){
                assert(remainad_initial_task_exec_time[i]==-1);
                continue;
            }else{
                remainad_initial_task_exec_time[i]--;
                if (remainad_initial_task_exec_time[i]<=0){
                    int c_task_id = initial_task_thread_queue[i];
                    assert(c_task_id==0);
                    int app_idx = app_idx_in_initial_thread_queue[i];
                    GraphTask &c_task = task_list[entrance_idx_in_NI][app_idx][0];
                    c_task.add_completed_times();
                    if (c_task.get_completed_times()<=c_task.get_required_times())
                        m_net_ptr->add_num_completed_tasks(app_idx, c_task.get_completed_times());

                    remainad_initial_task_exec_time[i] = -1;
                    initial_task_busy_flag[i] = false;
                    initial_task_thread_queue[i] = -1;
                }
            }
        }
    }
}

void
NetworkInterface::enqueueFlitsGeneratorBuffer( GraphEdge &e, int num, int task_execution_time ){
    //actually enqueue the head flit in the Buffer, if triggerred, the Buffer
    //generate the body flits, so Generator Buffer is acted as the Core.
    //would add remained execution time as parameter

    int current_core_id = e.get_src_proc_id();
    int dst_core_id = e.get_dst_proc_id();
    int dst_node_id = m_net_ptr->getNodeIdbyCoreId(dst_core_id);
    

    RouteInfo route;
    route.vnet = 2;
    route.src_ni = m_id;
    route.src_router = m_router_id;
    route.dest_ni = dst_node_id;
    //route.dest_router = route.dest_ni;
    route.dest_router = m_net_ptr->get_router_id(route.dest_ni);
    route.hops_traversed = -1;

    // Get vc_choice from edge
    route.vc_choice = e.get_vc_choice();
    /*
    std::vector<int> ddr_posi = m_net_ptr -> get_ddr_posi();
    int ddr_num = ddr_posi.size();
    for (int i=0;i<ddr_num;i++)
    {
        //if src or dst is ddr, choose 0 or 1 
        if((route.dest_router == ddr_posi[i])||(route.src_router == \
        ddr_posi[i]))
        {
            route.vc_choice = (route.dest_router >= route.src_router);
            break;
        }
        //if src or dst is not ddr, choose 2 or 3
        else
        {
            route.vc_choice = (route.dest_router >= route.src_router) \
            + 2;
        }
    }
    */

    //specify the destinition
    NetDest net_dest;
    NodeID destID = (NodeID) route.dest_ni;

    for (int m = 0;m < (int) MachineType_NUM; m++) {
        if ((destID >= MachineType_base_number((MachineType) m)) &&
                destID < MachineType_base_number((MachineType) (m+1))) {
            // calculating the NetDest associated with this dest_ni
            net_dest.clear();
            net_dest.add((MachineID) {(MachineType) m, (destID -
                        MachineType_base_number((MachineType) m))});
            //net_msg_ptr->getDestination() = net_dest;
            break;
        }
    }
    route.net_dest = net_dest;

    MsgPtr msg_ptr=NULL;

    //token length in packet
    int num_packets = ceil((double)num/m_net_ptr->getTokenLenInPkt());

    TGInfo tg;
    tg.src_task = e.get_src_task_id();
    tg.dest_task = e.get_dst_task_id();
    tg.edge_id = e.get_id();
    tg.token_id = e.get_current_token_id();
    tg.token_length_in_pkt = num_packets;
    tg.app_idx = e.get_app_idx();
    //DPRINTF(TaskGraph,"\n");
    /*
    DPRINTF(TaskGraph, "NI %d Task %d Edge %d equeue %d packets \
    in Generator Buffer.\n",\
                    m_id, tg.src_task, e.get_id() ,num_packets);
    */

    int temp_time_to_generate = 0;
    //generator_buffer_type *buffer_temp = new generator_buffer_type;
    for (int i = 0; i < num_packets; i++)
    {
        int num_flits = m_net_ptr->getTokenLenInPkt();

        if (i==num_packets-1){
            num_flits = num - (num_packets-1)*m_net_ptr->getTokenLenInPkt();
            num_flits = (num_flits >= m_net_ptr->getBuffersPerDataVC())\
            ?num_flits:m_net_ptr->getBuffersPerDataVC();
            /*DPRINTF(TaskGraph, " num flits = %d, buffer depth = %d\n ",\
            num_flits, m_net_ptr->getBuffersPerDataVC());
             */
        }

        //vnet2 is response vnet
        flit *fl = new flit(0, -1, 2, route, num_flits, \
        msg_ptr, curCycle(), tg);
        temp_time_to_generate += e.get_random_pkt_interval();
        if (temp_time_to_generate >= task_execution_time)
            temp_time_to_generate = task_execution_time;
        //use enqueue time as the time it should have been sent, for the src
        //delay in queue latency
        fl->set_enqueue_time(curCycle() + Cycles(temp_time_to_generate - 1));

        /*
        DPRINTF(TaskGraph,\
        "NI %d flit %d generate after %d cycles in Cycle [%lu]\n", m_id,\
        i, temp_time_to_generate, \
        u_int64_t(curCycle() + Cycles(temp_time_to_generate - 1)));
        */

        generator_buffer_type *buffer_temp = new generator_buffer_type;
        buffer_temp->flit_to_generate = fl;
        buffer_temp->time_to_generate_flit = temp_time_to_generate;
        int generator_buffer_idx = lookUpMap(m_core_id_index, current_core_id);
        generator_buffer[generator_buffer_idx].push_back( buffer_temp );
    }
}

// the Update Buffer action is simulated the pkt generation action when Core
// execute. Generator buffer to core buffer like write cache
void
NetworkInterface::updateGeneratorBuffer(){

    for (int i=0;i<m_num_cores;i++){

        int current_core_id = lookUpMap(m_index_core_id, i);

        for (unsigned j=0;j<generator_buffer[i].size();j++){
            //update remained sending time
            generator_buffer[i].at(j)->time_to_generate_flit--;
            if (generator_buffer[i].at(j)->time_to_generate_flit <= 0){
                flit *fl = generator_buffer[i].at(j)->flit_to_generate;
                //check whether the task on the core
                GraphTask& src_task = get_task_by_task_id(current_core_id, fl->get_tg_info().app_idx,\
                    fl->get_tg_info().src_task);
                GraphEdge& out_edge = src_task.\
                    get_outgoing_edge_by_eid(fl->get_tg_info().edge_id);
                int dst_core_id = out_edge.get_dst_proc_id();
                int dst_node_id = fl->get_route().dest_ni;

                if (dst_core_id == current_core_id){
                    //send to the same core, write directly
                    /*printf("Core [ %2d ] Task [ %2d ] send themseleves \
                    Task [[ %2d ]]!\n", current_core_id, \
                    fl->get_tg_info().src_task, fl->get_tg_info().dest_task);*/
                    assert(dst_node_id == m_id);
                    GraphTask& dst_task = get_task_by_task_id(current_core_id,fl->get_tg_info().app_idx,\
                        fl->get_tg_info().dest_task);
                    GraphEdge& in_edge = dst_task.\
                        get_incoming_edge_by_eid(fl->get_tg_info().edge_id);

                    if(!out_edge.record_sent_pkt(fl)){
                        continue;
                    }
/*
                    //record flit info in same core communication
                    int num_flits = fl->get_size();
                    vector<flit *> in_core_buffer;
                    for (int j=0;j<num_flits;j++){
                        flit* generated_fl = new flit(j, -1, 2, fl->get_route(), \
                        num_flits, fl->get_msg_ptr(), curCycle(), fl->get_tg_info());
                        //the fl enqueue time record the time flit should be sent
                        generated_fl->set_src_delay(curCycle() - \
                            fl->get_enqueue_time());

                        in_core_buffer.push_back(generated_fl);
                    }
                    delete fl;
                    for (int j=0;j<num_flits;j++){
                    //receive a pkt (just the head flit)
                        flit* fl = in_core_buffer.front();
                        fl->set_dequeue_time(curCycle());

                        if (fl->get_type() == TAIL_ || fl->get_type() == HEAD_TAIL_)
                            in_edge.record_pkt(fl, curCycle());    //operate in next task's in edge(in mem write)
                        //Note that!! if intra-cluster, we should set the hop_num
                        //to 0, because the intial value is -1 !
                        //record the flit time information !!
                        fl->increment_hops();
                        assert(fl->get_route().hops_traversed==0);
                        incrementStats(fl, true);
                        delete fl;
                        in_core_buffer.erase(in_core_buffer.begin());
                        in_core_buffer.shrink_to_fit();
                    }
*/
                    if (!in_edge.record_pkt(fl, curCycle()))    //operate in next task's in edge(in mem write)
                       printf("record pkt Error! \n");

                    //Note: if flit useless, remeber to delete !!
                    delete fl;
                }
                else {
                    if(dst_node_id == m_id)
                        core_buffer[i].push_back(fl); 
                    else
                        cluster_buffer[i].push_back(fl);
                }
                    
                delete generator_buffer[i].at(j);
                generator_buffer[i].erase(generator_buffer[i].begin()+j);
                generator_buffer[i].shrink_to_fit();
                j--;
            }
        }
    }
}

void
NetworkInterface::coreSendFlitsOut(){//in cluster, between core, crossbar delay time's up
    //core send flits out cluster via m_ni_out_vcs,
    //send flits in cluster via crossbar

    bool all_crossbar_busy = true; 
    for (int i=0;i<m_num_cores;i++){
        if (crossbar_delay_timer[i] != -1){
            //timer -1 means not used, not busy
            assert(crossbar_busy_out[i]);
            if (crossbar_delay_timer[i] == crossbar_delay){
                //send data from crossbar successfully
                crossbar_delay_timer[i] = -1;
                crossbar_busy_out[i] = false;

                flit* fl = crossbar_data[i].front();
                int num_flits = fl->get_size();
                assert(crossbar_data[i].size() == num_flits);

                //crossbar send flits to dst_core
                int dst_core_id = lookUpMap(m_index_core_id, i);

                GraphTask& dst_task = get_task_by_task_id(dst_core_id, fl->get_tg_info().app_idx,\
                    fl->get_tg_info().dest_task);
                GraphEdge& out_edge = dst_task.get_incoming_edge_by_eid(fl->get_tg_info().edge_id);
                assert(out_edge.get_dst_proc_id() == dst_core_id);

                for (int j=0;j<num_flits;j++){
                //receive a pkt (just the head flit)
                    flit* fl = crossbar_data[i].front();
                    fl->set_dequeue_time(curCycle());

                    if (fl->get_type() == TAIL_ || fl->get_type() == HEAD_TAIL_)
                        out_edge.record_pkt(fl, curCycle());    //operate in next task's in edge(in mem write)
                    //Note that!! if intra-cluster, we should set the hop_num
                    //to 0, because the intial value is -1 !
                    //record the flit time information !!
                    // fl->increment_hops();
                    // assert(fl->get_route().hops_traversed==0);
                    // incrementStats(fl);                        //if need to record intra cluster flit info, uncomment these 3 lines
                    delete fl;
                    crossbar_data[i].erase(crossbar_data[i].begin());
                    crossbar_data[i].shrink_to_fit();
                }
            }else{
                crossbar_delay_timer[i] = crossbar_delay_timer[i] + 1;
            }
        } else {
            all_crossbar_busy = false;
        }
    }

    if(m_num_cores>1 && !all_crossbar_busy)
        intraClusterOut();

    //Note! just return for vnet2
    int remained_num_vc = getNumRemainedIdleVC(2);
    for(int i=0;i<remained_num_vc;i++){        
        interClusterOut();

        if(calculateVC(2)==-1){
            assert(getNumRemainedIdleVC(2)==0);
            break;
        }
    }

    core_buffer_round_robin = (core_buffer_round_robin + 1) % m_num_cores;
}

void
NetworkInterface::intraClusterOut()
{
    for (int i=0;i<m_num_cores;i++){
        int j = (i + core_buffer_round_robin) % m_num_cores;
        int current_core_id = lookUpMap(m_index_core_id, j);
        assert(lookUpMap(m_core_id_index, current_core_id) == j);

        if (core_buffer[j].size() == 0)
            continue;

        // flit* fl = core_buffer[j].front();
        //////////////////////////////////////////////////////////////////////
        // int defu_app_idx = app_exec_rr[j] % m_num_apps;
        flit* defu_fl = core_buffer[j].front();
        // cout << core_buffer[j].size() << endl;
        int defu_app_idx = defu_fl->get_tg_info().app_idx;
        GraphTask& defu_src_task = get_task_by_task_id(current_core_id, defu_fl->get_tg_info().app_idx,defu_fl->get_tg_info().src_task);
        int p, least_c_e_times = defu_src_task.get_c_e_times(), pick = 0;

        for(p=0; p<core_buffer[j].size(); p++){
            flit* temp_fl = core_buffer[j].at(p);
            if(temp_fl->get_tg_info().app_idx != defu_app_idx){
                continue;
            }
            GraphTask& temp_src_task = get_task_by_task_id(current_core_id, temp_fl->get_tg_info().app_idx,temp_fl->get_tg_info().src_task);
            if(temp_src_task.get_c_e_times() < least_c_e_times){
                least_c_e_times = temp_src_task.get_c_e_times();
                pick = p;
            }
        }

        // int p, least_c_e_times = 99999, pick = 0, defu_app_idx;
        // if core_buffer do not have flit of this app, check next app. if have flit of this app, choose one with least c_e_times.
        srand((int)time(NULL));
        // for (int kk=0; kk<m_num_apps && least_c_e_times == 99999;kk++){
            
            // defu_app_idx = (kk+app_exec_rr[i]) % m_num_apps;
        // while(least_c_e_times == 99999){
        // //     defu_app_idx = app_exec_rr[j];
        //     if(m_num_apps == 1 || rand()%2 == 0){
        //         defu_app_idx = 0;
        //     }else{
        //         defu_app_idx = 1;
        //     }
        //     for(p=0; p<core_buffer[j].size(); p++){
        //         flit* temp_fl = core_buffer[j].at(p);
        //         if(temp_fl->get_tg_info().app_idx != defu_app_idx){
        //             continue;
        //         }
        //         GraphTask& temp_src_task = get_task_by_task_id(current_core_id, temp_fl->get_tg_info().app_idx,temp_fl->get_tg_info().src_task);
        //         if(temp_src_task.get_c_e_times() < least_c_e_times){
        //             least_c_e_times = temp_src_task.get_c_e_times();
        //             pick = p;
        //         }
        //     }
        //     // app_exec_rr[j] = (app_exec_rr[j] + 1) % m_num_apps;
        // }
        // if(least_c_e_times == 99999){
        //     continue;
        // }
        // app_exec_rr[j] = (app_exec_rr[j] + 1) % m_num_apps;
        
        //choose the least iteration task
        flit* fl = core_buffer[j].at(pick);
        //////////////////////////////////////////////////////////////////////
        GraphTask& src_task = get_task_by_task_id(current_core_id, fl->get_tg_info().app_idx,\
            fl->get_tg_info().src_task);
        int edge_id = fl->get_tg_info().edge_id;
        GraphEdge& out_edge = src_task.get_outgoing_edge_by_eid(edge_id);
        int dst_core_id = out_edge.get_dst_proc_id();
        
        int dst_core_idx = lookUpMap(m_core_id_index, dst_core_id);

/*
        //check in memory of dst task
        bool dstInMemoryFull = true;
        GraphTask& dst_task = get_task_by_task_id(dst_core_id, fl->get_tg_info().app_idx,\
                fl->get_tg_info().dest_task);
        GraphEdge& in_edge = dst_task.get_incoming_edge_by_eid(edge_id);
        if (in_edge.get_in_memory_remained() <= 0 && !in_edge.find_in_token_list(fl)){
            //printf("Crossbar: Task [%3d] In_Edge [%3d] from Task [%3d] in memory full ! Cannot receive pkt !",
                    //src_task.get_id(), edge_id, dst_task.get_id());
        } else {
            dstInMemoryFull = false;
        }
*/

        //because send to other core, so first record sent pkt
        //if (!crossbar_busy_out[dst_core_idx] && !dstInMemoryFull){
        if (!crossbar_busy_out[dst_core_idx]){
        //the output is not busy

            /*GraphTask& dst_task = get_task_by_task_id(dst_core_id, fl->get_tg_info().app_idx,\
                fl->get_tg_info().dest_task);
            GraphEdge& in_edge = dst_task.get_incoming_edge_by_eid(fl->get_tg_info().edge_id);
            if (in_edge.get_num_incoming_token() > not_used_token)
                continue;
            */

            if (!out_edge.record_sent_pkt(fl)){
                // cout << "NI: " << m_id << "    this core id: " << current_core_id << "    next core id: "<< dst_core_id << "    out edge's in mem_full\n";
                continue;
            }
            //record the token, after all pkt sent, out memory read pointer move

            int num_flits = fl->get_size();

            for (int j=0;j<num_flits;j++){
                flit* generated_fl = new flit(j, -1, 2, fl->get_route(), \
                num_flits, fl->get_msg_ptr(), curCycle(), fl->get_tg_info());
                //the fl enqueue time record the time flit should be sent
                generated_fl->set_src_delay(curCycle() - \
                    fl->get_enqueue_time());

                crossbar_data[dst_core_idx].push_back(generated_fl);
            }

            delete fl;
            //////////////////////////////////////////////////////////////////////
            core_buffer[j].erase(core_buffer[j].begin()+pick);
            //////////////////////////////////////////////////////////////////////
            // core_buffer[j].erase(core_buffer[j].begin());
            core_buffer[j].shrink_to_fit();
            core_buffer_sent[j] += 1;

            crossbar_busy_out[dst_core_idx] = true;
            crossbar_delay_timer[dst_core_idx] = 0;

            DPRINTF(TaskGraph, "Node [ %3d ] Core [ %3d ] Task %5d send \
                flit to Core [ %3d ] by Crossbar port [ %3d ]\n", \
                m_id, current_core_id, fl->get_tg_info().src_task, \
                dst_core_id, dst_core_idx); 
        }
    }
}

void
NetworkInterface::interClusterOut()
{
    for (int i=0;i<m_num_cores;i++){
        int j = (i + core_buffer_round_robin) % m_num_cores;
        int current_core_id = lookUpMap(m_index_core_id, j);
        assert(lookUpMap(m_core_id_index, current_core_id) == j);

        if (cluster_buffer[j].size() == 0)
            continue;

        // flit* fl = cluster_buffer[j].front();
        //////////////////////////////////////////////////////////////////////
        // int p, least_c_e_times = 99999, pick = 0, defu_app_idx;
        //if cluster_buffer do not have flit of this app, check next app. if have flit of this app, choose one with least c_e_times.
        srand((int)time(NULL));
        // for (int kk=0; kk<m_num_apps && least_c_e_times == 99999;kk++){
        //     defu_app_idx = (kk+app_exec_rr[i]) % m_num_apps;
        // while(least_c_e_times == 99999){
        // //     defu_app_idx = app_exec_rr[j];
        //     if(m_num_apps == 1 || rand()%2 == 0){
        //         defu_app_idx = 0;
        //     }else{
        //         defu_app_idx = 1;
        //     }
        //     for(p=0; p<cluster_buffer[j].size(); p++){
        //         flit* temp_fl = cluster_buffer[j].at(p);
        //         if(temp_fl->get_tg_info().app_idx != defu_app_idx){
        //             continue;
        //         }
        //         GraphTask& temp_src_task = get_task_by_task_id(current_core_id, temp_fl->get_tg_info().app_idx,temp_fl->get_tg_info().src_task);
        //         if(temp_src_task.get_c_e_times() < least_c_e_times){
        //             least_c_e_times = temp_src_task.get_c_e_times();
        //             pick = p;
        //         }
        //     }
        // if(least_c_e_times == 99999){
        //     continue;
        // }
            // app_exec_rr[j] = (app_exec_rr[j] + 1) % m_num_apps;
        // }
        // app_exec_rr[j] = (app_exec_rr[j] + 1) % m_num_apps;

        flit* defu_fl = cluster_buffer[j].front();
        int defu_app_idx = defu_fl->get_tg_info().app_idx;
        GraphTask& defu_src_task = get_task_by_task_id(current_core_id, defu_fl->get_tg_info().app_idx,defu_fl->get_tg_info().src_task);
        int p, least_c_e_times = defu_src_task.get_c_e_times(), pick = 0;
        for(p=0; p<cluster_buffer[j].size(); p++){
            flit* temp_fl = cluster_buffer[j].at(p);
            if(temp_fl->get_tg_info().app_idx != defu_app_idx){
                continue;
            }
            GraphTask& temp_src_task = get_task_by_task_id(current_core_id, temp_fl->get_tg_info().app_idx,temp_fl->get_tg_info().src_task);
            if(temp_src_task.get_c_e_times() < least_c_e_times){
                least_c_e_times = temp_src_task.get_c_e_times();
                pick = p;
            }
        }
        //choose the least iteration task
        flit* fl = cluster_buffer[j].at(pick);
        //////////////////////////////////////////////////////////////////////

        GraphTask& src_task = get_task_by_task_id(current_core_id, fl->get_tg_info().app_idx,fl->get_tg_info().src_task);
        GraphEdge& out_edge = src_task.get_outgoing_edge_by_eid(fl->get_tg_info().edge_id);
        int dst_core_id = out_edge.get_dst_proc_id();
        int dst_node_id = fl->get_route().dest_ni;

        int vc=calculateVC(fl->get_vnet());

        if (vc == -1)
            break;

        //check the buffer in the dest core

        if (!out_edge.record_sent_pkt(fl)){
            // cout << "NI: " << m_id << "    this core id: " << current_core_id << "    next NI:" << dst_node_id << "    next core id: "<< dst_core_id << "    out edge's in mem_full\n";
            continue;
        }else{
            // if (out_edge.get_id()==2){
            //     printf("Edge 2 send flit out\n");
            // }
        }
        // if(current_core_id == 4 || current_core_id == 1){
        //     std::cout << "current core: " << current_core_id << "\tnext core id: " << dst_core_id << "\tthis task id: " << src_task.get_id() << "\titeration: " << src_task.get_c_e_times() << endl;/////////////////////////////////////////
        // }
        
        DPRINTF(TaskGraph, "Node [ %3d ] Core [ %3d ] Task %5d send \
                flit to Node [ %3d ] Core [ %3d ]\n", m_id, current_core_id, \
                fl->get_tg_info().src_task, dst_node_id, dst_core_id);

        int num_flits = fl->get_size();

        for (int j=0;j<num_flits;j++){
            flit* generated_fl = new flit(j, vc, 2, fl->get_route(), \
            num_flits, fl->get_msg_ptr(),curCycle(), fl->get_tg_info());

            generated_fl->set_src_delay(curCycle() - fl->get_enqueue_time());

            m_ni_out_vcs[vc]->insert(generated_fl);
        }

        delete fl;
        //////////////////////////////////////////////////////////////////////
        cluster_buffer[j].erase(cluster_buffer[j].begin()+pick);
        //////////////////////////////////////////////////////////////////////

        // cluster_buffer[j].erase(cluster_buffer[j].begin());
        cluster_buffer[j].shrink_to_fit();
        core_buffer_sent[j] += 1;

        m_ni_out_vcs_enqueue_time[vc] = curCycle();
        m_out_vc_state[vc]->setState(ACTIVE_, curCycle());
    }
}

bool
NetworkInterface::configureNode(int num_cores, int* core_id, \
std::string* core_name, int* num_threads, int num_apps){
    m_num_cores = num_cores;

    for (int i=0;i<m_num_cores;i++){
        m_index_core_id.insert(make_pair(i, core_id[i]));
        m_core_id_index.insert(make_pair(core_id[i], i));
        m_core_id_name.insert(make_pair(core_id[i], core_name[i]));
        m_core_id_thread.insert(make_pair(core_id[i], num_threads[i]));
    }

    task_list.resize(m_num_cores);
    task_id_list.resize(m_num_cores);
    head_task_list.resize(m_num_cores);
    for (unsigned int i=0;i<task_list.size();i++){
        task_list[i].resize(num_apps);
        task_id_list[i].resize(num_apps);
        head_task_list[i].resize(num_apps);
    }

    m_num_apps = num_apps;
//    task_in_waiting_list.resize(m_num_cores);
    waiting_list_offset.resize(m_num_cores);

    remained_execution_time.resize(m_num_cores);
    generator_buffer.resize(m_num_cores);
    core_buffer.resize(m_num_cores);
    cluster_buffer.resize(m_num_cores);
    crossbar_busy_out.resize(m_num_cores);
    crossbar_delay_timer.resize(m_num_cores);
    crossbar_data.resize(m_num_cores);

//
    core_buffer_sent.resize(m_num_cores);

    for (int i=0;i<m_num_cores;i++){
        crossbar_busy_out[i] = false;
        crossbar_delay_timer[i] = -1;
        core_buffer_sent[i] = 0;
    }

    //configure thread
    task_in_thread_queue = new int*[m_num_cores];
    remained_execution_time_in_thread= new int*[m_num_cores];
    thread_busy_flag = new bool*[m_num_cores];
    task_to_exec_round_robin = new int*[m_num_cores];
    app_idx_in_thread_queue = new int*[m_num_cores];
    for (int i=0;i<m_num_cores;i++){
        task_in_thread_queue[i] = new int[num_threads[i]];
        remained_execution_time_in_thread[i] = new int[num_threads[i]];
        thread_busy_flag[i] = new bool[num_threads[i]];
        task_to_exec_round_robin[i] = new int [m_num_apps];
        app_idx_in_thread_queue[i] = new int[num_threads[i]];
    }

    //for multi-app
    app_exec_rr = new int [m_num_cores];

    //for throughput
    m_total_data_bits = new double [m_num_cores];

    for (int i=0;i<m_num_cores;i++){
        for (int j=0;j<num_threads[i];j++){
            task_in_thread_queue[i][j] = -1;
            remained_execution_time_in_thread[i][j] = -1;
            thread_busy_flag[i][j] = false;
            app_idx_in_thread_queue[i][j] = -1;
        }

        for (int j=0;j<m_num_apps;j++)
            task_to_exec_round_robin[i][j] = 0;

        app_exec_rr[i] = 0;
    }

    assert(num_cores==m_core_id_index.size()&& \
        num_cores==m_index_core_id.size()&& \
        num_cores==m_core_id_name.size()&& \
        num_cores==m_core_id_thread.size());

    // for back pressure
    // Note ! set size first
    input_buffer_size.resize(m_num_cores);
    for (int i=0;i<m_num_cores;i++){
        input_buffer_size[i] = 10;
    }

    input_buffer.resize(m_num_cores);
    for (int i=0;i<m_num_cores;i++){
        input_buffer[i].reserve(input_buffer_size[i]);
    }

    return true;
}

void
NetworkInterface::printNodeConfiguation(){
    cout<<"Node ID: "<<m_id<<"\tNumber of Cores: "<<m_num_cores<<endl;

    map<int, string>::iterator name_iter;
    int core_id;
    string core_name;
    int num_threads;

    for (name_iter = m_core_id_name.begin();\
        name_iter!=m_core_id_name.end();name_iter++){

        core_id = name_iter->first;
        core_name = name_iter->second;
        num_threads = lookUpMap(m_core_id_thread, core_id);
        cout<<"\tCore ID: "<<core_id<<"\tCore Name: "<<core_name\
            <<"\tNumber of Threads: "<<num_threads<<endl;
    }
}
