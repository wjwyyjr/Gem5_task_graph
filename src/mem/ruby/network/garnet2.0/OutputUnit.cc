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


#include "mem/ruby/network/garnet2.0/OutputUnit.hh"

#include "base/stl_helpers.hh"
#include "debug/RubyNetwork.hh"
#include "mem/ruby/network/garnet2.0/Credit.hh"
#include "mem/ruby/network/garnet2.0/Router.hh"

using namespace std;
using m5::stl_helpers::deletePointers;

OutputUnit::OutputUnit(int id, PortDirection direction, Router *router)
    : Consumer(router)
{
    m_id = id;
    m_direction = direction;
    m_router = router;
    m_num_vcs = m_router->get_num_vcs();
    m_vc_per_vnet = m_router->get_vc_per_vnet();
    m_out_buffer = new flitBuffer();
    m_vcs_for_allocation = m_router->get_vcs_for_allocation();
    m_vc_allocation_object = m_router->get_vc_allocation_object();

    for (int i = 0; i < m_num_vcs; i++) {
        m_outvc_state.push_back(new OutVcState(i, m_router->get_net_ptr()));
    }
}

OutputUnit::~OutputUnit()
{
    delete m_out_buffer;
    deletePointers(m_outvc_state);
}

void
OutputUnit::decrement_credit(int out_vc)
{
    DPRINTF(RubyNetwork, "Router %d OutputUnit %d decrementing credit for "
            "outvc %d at time: %lld\n",
            m_router->get_id(), m_id, out_vc, m_router->curCycle());

    m_outvc_state[out_vc]->decrement_credit();
}

void
OutputUnit::increment_credit(int out_vc)
{
    DPRINTF(RubyNetwork, "Router %d OutputUnit %d incrementing credit for "
            "outvc %d at time: %lld\n",
            m_router->get_id(), m_id, out_vc, m_router->curCycle());

    m_outvc_state[out_vc]->increment_credit();
}

// Check if the output VC (i.e., input VC at next router)
// has free credits (i..e, buffer slots).
// This is tracked by OutVcState
bool
OutputUnit::has_credit(int out_vc)
{
    assert(m_outvc_state[out_vc]->isInState(ACTIVE_, m_router->curCycle()));
    return m_outvc_state[out_vc]->has_credit();
}


// Check if the output port (i.e., input port at next router) has free VCs.
bool
OutputUnit::has_free_vc(int vnet)
{
    int vc_base = vnet*m_vc_per_vnet;
    for (int vc = vc_base; vc < vc_base + m_vc_per_vnet; vc++) {
        if (is_vc_idle(vc, m_router->curCycle()))
            return true;
    }

    return false;
}

// Assign a free output VC to the winner of Switch Allocation
int
OutputUnit::select_free_vc(int vnet)
{
    int vc_base = vnet*m_vc_per_vnet;
    for (int vc = vc_base; vc < vc_base + m_vc_per_vnet; vc++) {
        if (is_vc_idle(vc, m_router->curCycle())) {
            m_outvc_state[vc]->setState(ACTIVE_, m_router->curCycle());
            return vc;
        }
    }

    return -1;
}

//add for Ring

bool
OutputUnit::has_free_vc(int vnet, int vc_choice)
{
    int vc_base = vnet*m_vc_per_vnet;
    int vc_start, vc_end;
    
    if(m_vc_allocation_object != " " && m_vcs_for_allocation > 0){
    // allocate some vcs for specific object, 4 cases: 
    // special low; special high; normal low; normal high
        switch(vc_choice){
            case 0:
            // special low
                vc_start = 0;
                vc_end = m_vc_per_vnet / 2;
                // vc_end = m_vcs_for_allocation / 2;
                break;
            case 1:
            // special high
                // vc_start = m_vcs_for_allocation / 2;
                vc_start = m_vc_per_vnet / 2;
                vc_end = m_vc_per_vnet;
                break;
            case 2:
            // normal low
                vc_start = m_vcs_for_allocation;
                vc_end = (m_vcs_for_allocation + m_vc_per_vnet) / 2;
                break;
            case 3:
            // normal high
                vc_start = (m_vcs_for_allocation + m_vc_per_vnet) / 2;
                vc_end = m_vc_per_vnet;
                break;
            default:
                fatal("Vc choice error in has_free_vc()! Should in range [0, 3]!");
                break;
        }

        for (int vc = (vc_base + vc_start); vc < (vc_base + vc_end); vc++) {
            if (is_vc_idle(vc, m_router->curCycle()))
                return true;
        }

        return false;
    }
    else{
    // No special allocation. To realize deadlock-free, 2 cases:
    // low channel; high channel.
        int vc_base = vnet*m_vc_per_vnet;
        if(vc_choice == 0){
        // low channel
            vc_start = 0;
            vc_end = m_vc_per_vnet / 2;
        }
        else{
        // high channel
            assert(vc_choice == 1);
            vc_start = m_vc_per_vnet / 2;
            vc_end = m_vc_per_vnet;
        }
        for (int vc = (vc_base + vc_start); vc < (vc_base + vc_end); vc++) {
            if (is_vc_idle(vc, m_router->curCycle()))
                return true;
        }
        return false;
    }
}

// Assign a free output VC to the winner of Switch Allocation
int
OutputUnit::select_free_vc(int vnet, int vc_choice)
{
    int vc_base = vnet*m_vc_per_vnet;
    int vc_start, vc_end;
    
    if(m_vc_allocation_object != " " && m_vcs_for_allocation > 0){
    // allocate some vcs for specific object, 4 cases: 
    // special low; special high; normal low; normal high
        switch(vc_choice){
            case 0:
            // special low
                vc_start = 0;
                // vc_end = m_vcs_for_allocation / 2;
                vc_end = m_vc_per_vnet / 2;
                break;
            case 1:
            // special high
                // vc_start = m_vcs_for_allocation / 2;
                vc_start = m_vc_per_vnet / 2;
                vc_end = m_vc_per_vnet;
                break;
            case 2:
            // normal low
                vc_start = m_vcs_for_allocation;
                vc_end = (m_vcs_for_allocation + m_vc_per_vnet) / 2;
                break;
            case 3:
            // normal high
                vc_start = (m_vcs_for_allocation + m_vc_per_vnet) / 2;
                vc_end = m_vc_per_vnet;
                break;
            default:
                fatal("Vc choice error in has_free_vc()! Should in range [0, 3]!");
                break;
        }
        for (int vc = (vc_base + vc_start); vc < (vc_base + vc_end); vc++) {
            if (is_vc_idle(vc, m_router->curCycle())) {
                m_outvc_state[vc]->setState(ACTIVE_, m_router->curCycle());
                return vc;
            }
        }

        return -1;
    }
    else{
    // Normal situation.
    // No special allocation. To realize deadlock-free, 2 cases:
    // low channel; high channel.
        int vc_base = vnet*m_vc_per_vnet;
        if(vc_choice == 0){
        // low channel
            vc_start = 0;
            vc_end = m_vc_per_vnet / 2;
        }
        else{
        // high channel
            assert(vc_choice == 1);
            vc_start = m_vc_per_vnet / 2;
            vc_end = m_vc_per_vnet;
        }
        for (int vc = (vc_base + vc_start); vc < (vc_base + vc_end); vc++) {
            if (is_vc_idle(vc, m_router->curCycle())){
                m_outvc_state[vc]->setState(ACTIVE_, m_router->curCycle());
                return vc;
            }
        }
        return -1;
    }   
}

// bool
// OutputUnit::has_free_vc(int vnet, int vc_choice)
// {
//     int vc_base = vnet*m_vc_per_vnet;
//     int vc = vc_base + vc_choice;
//     if (is_vc_idle(vc, m_router->curCycle()))//only check vc_choice
//         return true;
//     //find the vc number of its own net, for example for vnet1,
//     //vc num begin with 1*2=2 to 1*2+2==4
//     return false;
// }

// int
// OutputUnit::select_free_vc(int vnet, int vc_choice)
// {
//     int vc_base = vnet*m_vc_per_vnet;
//     int vc = vc_base + vc_choice;
//     if (is_vc_idle(vc, m_router->curCycle()))//only check vc_choice
//     {
//         m_outvc_state[vc]->setState(ACTIVE_, m_router->curCycle());
//         return vc;
//     }
//     //find free vc and set outvc active VIP!!!
//     //HERE we set vc vc_choice and always check if it is idle
//     return -1;
// }

/*
 * The wakeup function of the OutputUnit reads the credit signal from the
 * downstream router for the output VC (i.e., input VC at downstream router).
 * It increments the credit count in the appropriate output VC state.
 * If the credit carries is_free_signal as true,
 * the output VC is marked IDLE.
 */

void
OutputUnit::wakeup()
{
    if (m_credit_link->isReady(m_router->curCycle())) {
        Credit *t_credit = (Credit*) m_credit_link->consumeLink();
        increment_credit(t_credit->get_vc());

        if (t_credit->is_free_signal())
            set_vc_state(IDLE_, t_credit->get_vc(), m_router->curCycle());

        delete t_credit;
    }
}

flitBuffer*
OutputUnit::getOutQueue()
{
    return m_out_buffer;
}

void
OutputUnit::set_out_link(NetworkLink *link)
{
    m_out_link = link;
}

void
OutputUnit::set_credit_link(CreditLink *credit_link)
{
    m_credit_link = credit_link;
}

uint32_t
OutputUnit::functionalWrite(Packet *pkt)
{
    return m_out_buffer->functionalWrite(pkt);
}
