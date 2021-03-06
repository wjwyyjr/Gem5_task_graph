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


#ifndef __MEM_RUBY_NETWORK_GARNET2_0_GARNETNETWORK_HH__
#define __MEM_RUBY_NETWORK_GARNET2_0_GARNETNETWORK_HH__

#include <iostream>
#include <vector>

#include "base/output.hh"
#include "debug/TaskGraph.hh"
#include "mem/ruby/common/Consumer.hh"
#include "mem/ruby/network/Network.hh"
#include "mem/ruby/network/fault_model/FaultModel.hh"
#include "mem/ruby/network/garnet2.0/CommonTypes.hh"
#include "mem/ruby/network/garnet2.0/GraphTask.hh"
#include "params/GarnetNetwork.hh"
#include "sim/sim_exit.hh"

class FaultModel;
class NetworkInterface;
class Router;
class NetDest;
class NetworkLink;
class CreditLink;

class GarnetNetwork : public Network, public Consumer
{
  public:
    typedef GarnetNetworkParams Params;
    GarnetNetwork(const Params *p);

    ~GarnetNetwork();
    void init();
    //add for task graph
    void wakeup();
    void scheduleWakeupAbsolute(Cycles time);

    // Configuration (set externally)

    // for 2D topology
    int getNumRows() const { return m_num_rows; }
    int getNumCols() { return m_num_cols; }

    // for network
    uint32_t getNiFlitSize() const { return m_ni_flit_size; }
    uint32_t getVCsPerVnet() const { return m_vcs_per_vnet; }
    uint32_t getBuffersPerDataVC() { return m_buffers_per_data_vc; }
    uint32_t getBuffersPerCtrlVC() { return m_buffers_per_ctrl_vc; }
    int getRoutingAlgorithm() const { return m_routing_algorithm; }

    bool isFaultModelEnabled() const { return m_enable_fault_model; }
    FaultModel* fault_model;

    //for Task Graph
    bool isTaskGraphEnabled() { return m_task_graph_enable; }
    std::string getTaskGraphFilename() { return m_task_graph_file; }
    int getTokenLenInPkt() { return m_token_packet_length; }

    bool loadTraffic(std::string filename);
    bool checkApplicationFinish();
    //for construct architecture in task graph mode
    bool constructArchitecture(std::string filename);
    int getNodeIdbyCoreId(int core_id);
    //for read the application configuration
    bool readApplicationConfig(std::string filename);

    bool IsPrintTaskExecuInfo(){return m_print_task_execution_info;}

    void PrintAppDelay();
    void PrintTaskWaitingInfo();

    //for Ring Topology
    std::string getTopology() { return m_topology; }

    // Internal configuration
    bool isVNetOrdered(int vnet) const { return m_ordered[vnet]; }
    VNET_type
    get_vnet_type(int vc)
    {
        int vnet = vc/getVCsPerVnet();
        return m_vnet_type[vnet];
    }
    int getNumRouters();
    int get_router_id(int ni);
    //record PE-7 position for initial task judgement in NI
    int get_entrance_NI(){ return entrance_NI; }
    int get_entrance_core(){ return entrance_core; }
    int get_entrance_idx_in_NI(){ return entrance_idx_in_NI; }
    int get_m_num_application(){ return m_num_application; }


    // Methods used by Topology to setup the network
    void makeExtOutLink(SwitchID src, NodeID dest, BasicLink* link,
                     const NetDest& routing_table_entry);
    void makeExtInLink(NodeID src, SwitchID dest, BasicLink* link,
                    const NetDest& routing_table_entry);
    void makeInternalLink(SwitchID src, SwitchID dest, BasicLink* link,
                          const NetDest& routing_table_entry,
                          PortDirection src_outport_dirn,
                          PortDirection dest_inport_dirn);

    //! Function for performing a functional write. The return value
    //! indicates the number of messages that were written.
    uint32_t functionalWrite(Packet *pkt);

    // Stats
    void collateStats();
    void regStats();
    void print(std::ostream& out) const;

    // increment counters
    void increment_injected_packets(int vnet) { m_packets_injected[vnet]++; }
    void increment_received_packets(int vnet) { m_packets_received[vnet]++; }

    void
    increment_packet_network_latency(Cycles latency, int vnet)
    {
        m_packet_network_latency[vnet] += latency;
    }

    void
    increment_packet_queueing_latency(Cycles latency, int vnet)
    {
        m_packet_queueing_latency[vnet] += latency;
    }

    void increment_injected_flits(int vnet) { m_flits_injected[vnet]++; }
    void increment_received_flits(int vnet) { m_flits_received[vnet]++; }

    void
    increment_flit_network_latency(Cycles latency, int vnet)
    {
        m_flit_network_latency[vnet] += latency;
    }

    void
    increment_flit_queueing_latency(Cycles latency, int vnet)
    {
        m_flit_queueing_latency[vnet] += latency;
    }

    void
    increment_total_hops(int hops)
    {
        m_total_hops += hops;
    }

    void
    add_execution_time_to_total(int ex_time)
    {
        m_total_task_execution_time += ex_time;
    }

    //record the latenty for src and dst
    void
    add_src_dst_latency(int src, int dst, int latency){
        src_dst_latency[src][dst] += latency;
    }

    //add the task to num_completed_tasks
    //To output the ete delay when run simulation
    //shoulb ensure not over m_applicaton_execution_iterations[app_idx]
    void
    add_num_completed_tasks(int app_idx, int ex_iters){
        //Note here! ex_iters should minus 1
        num_completed_tasks[app_idx][ex_iters-1]++;
        if(num_completed_tasks[app_idx][ex_iters-1] == m_num_task[app_idx]){
            //if num_tasks satisfied, then we can ensure the iteration had done.
            current_execution_iterations[app_idx]++;
            assert(ex_iters==current_execution_iterations[app_idx]);
            output_ete_delay(app_idx, ex_iters-1);
        }
    }

    //update start, end time
    void
    update_start_end_time(int app_idx, int ex_iters, int start, int end){
        //compare the task start time and end time
        task_start_time[app_idx][ex_iters] = (start<task_start_time[app_idx][ex_iters])?\
            start:task_start_time[app_idx][ex_iters];
        task_end_time[app_idx][ex_iters] = (end>task_end_time[app_idx][ex_iters])?\
            end:task_end_time[app_idx][ex_iters];
    }

    //print ete-delay for certain iteration of one application
    void output_ete_delay(int app_idx, int ex_iters);

    //for debug
    OutputStream *task_start_time_vs_id;
    OutputStream *task_start_end_time_vs_id;
    OutputStream *task_start_time_vs_id_iters;
    //for the ete delay
    OutputStream *throughput_info;
    OutputStream *app_delay_running_info;
    // OutputStream *start_time_info;
    // OutputStream *end_time_info;
    // OutputStream *ete_info;

    OutputStream *network_performance_info;
    //for the task waiting time
    OutputStream *task_waiting_time_info;
    //
    bool back_pressure(int m_id);
    // update the in memory remianed information for the src task in src core when record pkt
    void update_in_memory_info(int core_id, int app_idx, int src_task_id, int edge_id);

    // find greatest common divisor, for ratio token in NI
    int gcd(int a, int b){
        return b ? gcd(b,a%b):a;
    }
    std::vector<int> get_ratio_token(int *iterations);

  protected:
    // Configuration
    int m_num_rows;
    int m_num_cols;
    uint32_t m_ni_flit_size;
    uint32_t m_vcs_per_vnet;
    uint32_t m_buffers_per_ctrl_vc;
    uint32_t m_buffers_per_data_vc;
    int m_routing_algorithm;
    bool m_enable_fault_model;
    bool m_task_graph_enable;
    std::string m_task_graph_file;
    int m_token_packet_length;
    std::string m_topology;
    std::string m_architecture_file;
    bool m_print_task_execution_info;
    uint32_t m_vcs_for_allocation;
    std::string m_vc_allocation_object;

    //for task graph
    int m_num_proc;
    int* m_num_task;
    int* m_num_edge;
    int* m_num_head_task;
    std::vector<std::vector<int> > task_start_time;
    std::vector<std::vector<int> > task_end_time;
    std::vector<std::vector<int> > ETE_delay;
    std::vector<std::vector<int> > head_task;
    //for construct architecture in task graph mode
    std::map<int, int> m_core_id_node_id; //core_id -> node_id
    //for multi-application traffic
    int m_num_application;
    int m_total_execution_iterations;
    std::string* m_application_name;
    int* m_applicaton_execution_iterations;
    //for print ete_delay when run simulation
    int* current_execution_iterations;
    int** num_completed_tasks; 
    //for record point to point pkt lantency
    int m_num_core;
    int** src_dst_latency;
    //for vc to check vc_allocation_object position
    std::vector<int> vc_allocation_object_position;
    //record PE-7 position for initial task judgement in NI
    int entrance_NI;
    int entrance_core;
    int entrance_idx_in_NI;

    // Statistical variables
    Stats::Vector m_packets_received;
    Stats::Vector m_packets_injected;
    Stats::Vector m_packet_network_latency;
    Stats::Vector m_packet_queueing_latency;

    Stats::Formula m_avg_packet_vnet_latency;
    Stats::Formula m_avg_packet_vqueue_latency;
    Stats::Formula m_avg_packet_network_latency;
    Stats::Formula m_avg_packet_queueing_latency;
    Stats::Formula m_avg_packet_latency;

    Stats::Vector m_flits_received;
    Stats::Vector m_flits_injected;
    Stats::Vector m_flit_network_latency;
    Stats::Vector m_flit_queueing_latency;

    Stats::Formula m_avg_flit_vnet_latency;
    Stats::Formula m_avg_flit_vqueue_latency;
    Stats::Formula m_avg_flit_network_latency;
    Stats::Formula m_avg_flit_queueing_latency;
    Stats::Formula m_avg_flit_latency;

    Stats::Scalar m_total_ext_in_link_utilization;
    Stats::Scalar m_total_ext_out_link_utilization;
    Stats::Scalar m_total_int_link_utilization;
    Stats::Scalar m_average_link_utilization;
    Stats::Vector m_average_vc_load;

    Stats::Scalar  m_total_hops;
    Stats::Formula m_avg_hops;

    //add for TG
    Stats::Scalar m_total_task_execution_time;

    int m_in_mem_size;
    int m_out_mem_size;

  private:
    GarnetNetwork(const GarnetNetwork& obj);
    GarnetNetwork& operator=(const GarnetNetwork& obj);

    std::vector<VNET_type > m_vnet_type;
    std::vector<Router *> m_routers;   // All Routers in Network
    std::vector<NetworkLink *> m_networklinks; // All flit links in the network
    std::vector<CreditLink *> m_creditlinks; // All credit links in the network
    std::vector<NetworkInterface *> m_nis;   // All NI's in Network
};

inline std::ostream&
operator<<(std::ostream& out, const GarnetNetwork& obj)
{
    obj.print(out);
    out << std::flush;
    return out;
}

#endif //__MEM_RUBY_NETWORK_GARNET2_0_GARNETNETWORK_HH__
